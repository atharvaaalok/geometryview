#include "ipc/import_job.h"

#include "ipc/cache_eviction.h"

#include "base/types.h"
#include "format/blob.h"
#include "format/helper_messages.h"

#define XXH_INLINE_ALL
#include "base/xxhash.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace geo::ipc {
namespace {

constexpr int kSourceFd = 3;
constexpr int kOutputFd = 4;
constexpr int kFirstHighFd = 10; // above every number the helper's descriptors take

// Closes a file descriptor when it goes out of scope.
class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

bool readExactly(int fd, void* data, usize size) {
    auto* cursor = static_cast<u8*>(data);
    while (size != 0) {
        const ssize_t count = read(fd, cursor, size);
        if (count > 0) {
            cursor += count;
            size -= usize(count);
        } else if (count == 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

// Starts the helper with exactly three inherited descriptors beyond stderr.
// POSIX_SPAWN_CLOEXEC_DEFAULT closes everything else, so it cannot reach any other file this
// process has open.
bool spawnHelper(const std::string& helper, const std::vector<std::string>& options, int sourceFd,
                 int outputFd, int resultFd, pid_t& pid, std::string& error) {
    // The descriptors are duplicated above the numbers they become first: if one were already 3
    // or 4, an earlier dup2 would overwrite it before it was moved, and the helper would write its
    // cache file into the source.
    const ScopedFd highSource(fcntl(sourceFd, F_DUPFD_CLOEXEC, kFirstHighFd));
    const ScopedFd highOutput(fcntl(outputFd, F_DUPFD_CLOEXEC, kFirstHighFd));
    const ScopedFd highResult(fcntl(resultFd, F_DUPFD_CLOEXEC, kFirstHighFd));
    if (highSource.get() < 0 || highOutput.get() < 0 || highResult.get() < 0) {
        error = std::format("The import helper could not start: {}.", std::strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, highSource.get(), kSourceFd);
    posix_spawn_file_actions_adddup2(&actions, highOutput.get(), kOutputFd);
    posix_spawn_file_actions_adddup2(&actions, highResult.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

    const std::string source = std::to_string(kSourceFd), output = std::to_string(kOutputFd);
    std::vector<char*> arguments = {const_cast<char*>(helper.c_str()),
                                    const_cast<char*>(source.c_str()),
                                    const_cast<char*>(output.c_str())};
    for (const std::string& option : options) {
        arguments.push_back(const_cast<char*>(option.c_str()));
    }
    arguments.push_back(nullptr);
    const int status =
        posix_spawn(&pid, helper.c_str(), &actions, &attributes, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
        error = std::format("The import helper could not start: {}.", std::strerror(status));
        return false;
    }
    return true;
}

} // namespace

std::string cachePathFor(const std::string& sourcePath, const ImportPaths& paths,
                         std::string& error) {
    struct stat info{};
    if (stat(sourcePath.c_str(), &info) != 0) {
        error = errno == ENOENT
                    ? "This file could not be found."
                    : std::format("This file could not be read: {}.", std::strerror(errno));
        return {};
    }
    const std::string identity = std::format(
        "{}|{}|{}|{}|{}.{}|{}", sourcePath, u64(info.st_dev), u64(info.st_ino), i64(info.st_size),
        i64(info.st_mtimespec.tv_sec), i64(info.st_mtimespec.tv_nsec), format::kVersion);
    const u64 key = XXH3_64bits(identity.data(), identity.size());
    return std::format("{}/{:016x}.geoblob", paths.cacheDirectory, key);
}

namespace {

// Delivers every segment of a finished cache file. Returns false if the file is damaged.
bool deliverCached(const std::string& blobPath, ImportListener& listener) {
    ScopedFd cached(open(blobPath.c_str(), O_RDONLY));
    if (cached.get() < 0) {
        return false;
    }
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    if (!scene::openCacheFile(cached.get(), segments, error)) {
        return false;
    }
    for (std::shared_ptr<scene::Segment>& segment : segments) {
        listener.segmentReady(std::move(segment));
    }
    return true;
}

// Finer levels saved for a cache file, coarsest first, so finer ones replace them as they load.
std::vector<std::string> refinementsOf(const std::string& blobPath) {
    const usize slash = blobPath.rfind('/');
    const std::string directory = blobPath.substr(0, slash);
    const std::string prefix = blobPath.substr(slash + 1, blobPath.size() - slash - 1 -
                                                              std::string_view(".geoblob").size()) +
                               ".r";
    std::vector<std::string> names;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* entry = readdir(listing)) {
            const std::string_view name(entry->d_name);
            if (name.starts_with(prefix) && name.ends_with(".geoblob")) {
                names.emplace_back(name);
            }
        }
        closedir(listing);
    }
    std::sort(names.begin(), names.end());
    for (std::string& name : names) {
        name = directory + "/" + name;
    }
    return names;
}

// Runs the helper to write `blobPath`, passing its segments on as they arrive.
ImportOutcome runHelper(const std::string& sourcePath, const std::string& blobPath,
                        const ImportPaths& paths, const std::vector<std::string>& options,
                        ImportListener& listener, ImportCancel* cancel) {
    ImportOutcome outcome;
    outcome.blobPath = blobPath;
    ScopedFd source(open(sourcePath.c_str(), O_RDONLY));
    if (source.get() < 0) {
        outcome.message = "This file could not be opened.";
        return outcome;
    }
    std::string temporary = blobPath + ".XXXXXX";
    ScopedFd output(mkstemp(temporary.data()));
    if (output.get() < 0) {
        outcome.message = std::format("The cache could not be written: {}.", std::strerror(errno));
        return outcome;
    }
    std::array<int, 2> pipeEnds{};
    if (pipe(pipeEnds.data()) != 0) {
        outcome.message = "The import helper could not be connected.";
        unlink(temporary.c_str());
        return outcome;
    }
    ScopedFd readEnd(pipeEnds[0]), writeEnd(pipeEnds[1]);

    pid_t pid = 0;
    if (!spawnHelper(paths.helper, options, source.get(), output.get(), writeEnd.get(), pid,
                     outcome.message)) {
        unlink(temporary.c_str());
        return outcome;
    }
    writeEnd.reset(); // otherwise reading would never see end of file
    if (cancel != nullptr) {
        cancel->setHelper(pid);
    }
    bool done = false, failed = false;
    std::string failure;
    while (!done && !failed) {
        format::MessageHeader header{};
        if (!readExactly(readEnd.get(), &header, sizeof(header)) ||
            header.size > format::kMaxMessagePayload) {
            break;
        }
        std::array<char, format::kMaxMessagePayload> payload;
        if (!readExactly(readEnd.get(), payload.data(), header.size)) {
            break;
        }
        switch (format::HelperMessage(header.kind)) {
        case format::HelperMessage::Segment: {
            format::SegmentMessage message{};
            if (header.size != sizeof(message)) {
                failed = true;
                break;
            }
            std::memcpy(&message, payload.data(), sizeof(message));
            std::string error;
            std::shared_ptr<scene::Segment> segment =
                scene::Segment::open(output.get(), message.offset, message.size, error);
            if (!segment) {
                failed = true;
                failure = error;
                break;
            }
            listener.segmentReady(std::move(segment));
            break;
        }
        case format::HelperMessage::Progress: {
            format::ProgressMessage message{};
            if (header.size == sizeof(message)) {
                std::memcpy(&message, payload.data(), sizeof(message));
                listener.progress(message.done, message.total);
            }
            break;
        }
        case format::HelperMessage::Warning:
            listener.warning(std::string(payload.data(), header.size));
            break;
        case format::HelperMessage::Done:
            done = true;
            break;
        case format::HelperMessage::Failed:
            failed = true;
            failure.assign(payload.data(), header.size);
            break;
        }
    }
    if (failed && failure.empty()) {
        failure = "The import helper sent something unexpected.";
    }
    if (failed) {
        kill(pid, SIGKILL);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (cancel != nullptr) {
        cancel->setHelper(0);
    }
    if (cancel != nullptr && cancel->cancelled()) {
        outcome.message = "The import was cancelled.";
        unlink(temporary.c_str());
        return outcome;
    }
    if (!done) {
        outcome.message = !failure.empty()
                              ? failure
                              : "The import helper stopped unexpectedly. This file may be damaged.";
        unlink(temporary.c_str());
        return outcome;
    }
    if (rename(temporary.c_str(), blobPath.c_str()) != 0) {
        outcome.message = std::format("The cache could not be saved: {}.", std::strerror(errno));
        unlink(temporary.c_str());
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

} // namespace

ImportOutcome importFile(const std::string& sourcePath, const ImportPaths& paths,
                         ImportListener& listener, ImportCancel* cancel,
                         const ImportOptions& options) {
    ImportOutcome outcome;
    const std::string blobPath = cachePathFor(sourcePath, paths, outcome.message);
    if (blobPath.empty()) {
        return outcome;
    }
    if (deliverCached(blobPath, listener)) {
        touchCacheFile(blobPath); // just used, so eviction keeps it longest
        // Finer levels from earlier sessions come back too, so a part zoomed into before is sharp
        // at once.
        for (const std::string& refinement : refinementsOf(blobPath)) {
            if (!deliverCached(refinement, listener)) {
                unlink(refinement.c_str());
            }
        }
        outcome.ok = true;
        outcome.fromCache = true;
        outcome.blobPath = blobPath;
        return outcome;
    }
    // A damaged cache, and anything refined from it, must not stop the file opening.
    unlink(blobPath.c_str());
    for (const std::string& refinement : refinementsOf(blobPath)) {
        unlink(refinement.c_str());
    }
    std::vector<std::string> flags;
    if (options.workers != 0) {
        flags = {"--workers", std::to_string(options.workers)};
    }
    return runHelper(sourcePath, blobPath, paths, flags, listener, cancel);
}

bool isCached(const std::string& sourcePath, const ImportPaths& paths) {
    std::string error;
    const std::string blobPath = cachePathFor(sourcePath, paths, error);
    struct stat info{};
    return !blobPath.empty() && stat(blobPath.c_str(), &info) == 0;
}

ImportOutcome readStructure(const std::string& sourcePath, const ImportPaths& paths,
                            ImportListener& listener, ImportCancel* cancel) {
    ImportOutcome outcome;
    const std::string blobPath = cachePathFor(sourcePath, paths, outcome.message);
    if (blobPath.empty()) {
        return outcome;
    }
    // Beside the file's own entry, so eviction counts it with it, and removed once read: its
    // segments stay mapped.
    const std::string stem =
        blobPath.substr(0, blobPath.size() - std::string_view(".geoblob").size());
    const std::string structurePath = stem + ".structure.geoblob";
    outcome = runHelper(sourcePath, structurePath, paths, {"--structure", "--workers", "1"},
                        listener, cancel);
    unlink(structurePath.c_str());
    outcome.blobPath.clear();
    return outcome;
}

ImportOutcome refineFile(const std::string& sourcePath, const ImportPaths& paths, u32 level,
                         std::span<const u32> definitions, ImportListener& listener,
                         ImportCancel* cancel) {
    ImportOutcome outcome;
    const std::string basePath = cachePathFor(sourcePath, paths, outcome.message);
    if (basePath.empty() || definitions.empty()) {
        return outcome;
    }
    std::string list;
    for (const u32 definition : definitions) {
        list += std::format("{}{}", list.empty() ? "" : ",", definition);
    }
    const u64 key = XXH3_64bits(list.data(), list.size());
    const std::string stem =
        basePath.substr(0, basePath.size() - std::string_view(".geoblob").size());
    const std::string blobPath = std::format("{}.r{}-{:016x}.geoblob", stem, level, key);
    if (deliverCached(blobPath, listener)) {
        outcome.ok = true;
        outcome.fromCache = true;
        outcome.blobPath = blobPath;
        return outcome;
    }
    return runHelper(sourcePath, blobPath, paths,
                     {"--level", std::to_string(level), "--definitions", list}, listener, cancel);
}

struct ImportCancel::State {
    std::mutex mutex;
    pid_t helper = 0;
    bool cancelled = false;
};

ImportCancel::ImportCancel() : state_(std::make_shared<State>()) {}

void ImportCancel::cancel() {
    std::lock_guard lock(state_->mutex);
    state_->cancelled = true;
    if (state_->helper != 0) {
        kill(state_->helper, SIGTERM);
    }
}

void ImportCancel::setHelper(int pid) {
    std::lock_guard lock(state_->mutex);
    state_->helper = pid;
    if (pid != 0 && state_->cancelled) {
        kill(pid, SIGTERM);
    }
}

bool ImportCancel::cancelled() const {
    std::lock_guard lock(state_->mutex);
    return state_->cancelled;
}

} // namespace geo::ipc
