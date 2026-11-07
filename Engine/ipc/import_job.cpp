#include "ipc/import_job.h"

#include "base/types.h"
#include "format/blob.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <spawn.h>
#include <string_view>
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

// Reads the helper's result line, which is short, until the helper closes its end.
std::string readAll(int fd) {
    std::string text;
    std::array<char, 512> buffer{};
    while (true) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            text.append(buffer.data(), usize(count));
        } else if (count == 0 || errno != EINTR) {
            return text;
        }
    }
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

ImportOutcome importFile(const std::string& sourcePath, const ImportPaths& paths) {
    ImportOutcome outcome;
    const std::string blobPath = cachePathFor(sourcePath, paths, outcome.message);
    if (blobPath.empty()) {
        return outcome;
    }
    outcome.blobPath = blobPath;
    if (access(blobPath.c_str(), R_OK) == 0) {
        outcome.ok = true;
        return outcome;
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
    const std::string result = readAll(readEnd.get());

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status)) {
        outcome.message = "The import helper stopped unexpectedly. This file may be damaged.";
        unlink(temporary.c_str());
        return outcome;
    }
    if (WEXITSTATUS(status) != 0 || !result.starts_with("ok")) {
        constexpr std::string_view kPrefix = "error ";
        outcome.message = result.starts_with(kPrefix)
                              ? result.substr(kPrefix.size(), result.find('\n') - kPrefix.size())
                              : "The file could not be imported.";
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

} // namespace geo::ipc
