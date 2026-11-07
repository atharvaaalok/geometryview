#include "ipc/import_job.h"

#include "base/types.h"
#include "format/blob.h"
#include "format/helper_messages.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace geo::ipc {
namespace {

constexpr int kSourceFd = 3;
constexpr int kOutputFd = 4;

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
bool spawnHelper(const std::string& helper, int sourceFd, int outputFd, int resultFd, pid_t& pid,
                 std::string& error) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sourceFd, kSourceFd);
    posix_spawn_file_actions_adddup2(&actions, outputFd, kOutputFd);
    posix_spawn_file_actions_adddup2(&actions, resultFd, STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

    const std::string source = std::to_string(kSourceFd), output = std::to_string(kOutputFd);
    std::array<char*, 4> arguments = {const_cast<char*>(helper.c_str()),
                                      const_cast<char*>(source.c_str()),
                                      const_cast<char*>(output.c_str()), nullptr};
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

ImportOutcome importFile(const std::string& sourcePath, const ImportPaths& paths,
                         ImportListener& listener, ImportCancel* cancel) {
    ImportOutcome outcome;
    const std::string blobPath = cachePathFor(sourcePath, paths, outcome.message);
    if (blobPath.empty()) {
        return outcome;
    }
    outcome.blobPath = blobPath;

    if (ScopedFd cached(open(blobPath.c_str(), O_RDONLY)); cached.get() >= 0) {
        std::vector<std::shared_ptr<scene::MappedSegment>> segments;
        std::string error;
        if (scene::openCacheFile(cached.get(), segments, error)) {
            for (std::shared_ptr<scene::MappedSegment>& segment : segments) {
                listener.segmentReady(std::move(segment));
            }
            outcome.ok = true;
            outcome.fromCache = true;
            return outcome;
        }
        // A damaged cache must not stop the file opening: import it again.
        unlink(blobPath.c_str());
    }

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
    if (!spawnHelper(paths.helper, source.get(), output.get(), writeEnd.get(), pid,
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
            std::shared_ptr<scene::MappedSegment> segment =
                scene::MappedSegment::open(output.get(), message.offset, message.size, error);
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
