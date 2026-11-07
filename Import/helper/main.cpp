// geometry-import: the sandboxed helper that parses source files.
//
// Untrusted files are parsed here, in a process of its own, so a malformed file can only crash this
// process, its memory returns to the system when it exits, and the viewer never links a parser.
//
// Usage: geometry-import <source-fd> <output-fd>. Results are framed messages on stdout, in
// format/helper_messages.h. OCCT runs in a further process, next to this executable.
#include "format/helper_messages.h"
#include "import.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace geo;
using format::HelperMessage;

class MessageWriter final : public import::SegmentListener {
public:
    void send(HelperMessage kind, const void* payload, usize size) {
        const format::MessageHeader header{u32(kind), u32(size)};
        std::lock_guard lock(mutex_);
        writeAll(&header, sizeof(header));
        writeAll(payload, size);
    }

    void segmentWritten(u64 offset, u64 size) override {
        const format::SegmentMessage message{offset, size};
        send(HelperMessage::Segment, &message, sizeof(message));
    }

    void progress(u32 done, u32 total) override {
        const format::ProgressMessage message{done, total};
        send(HelperMessage::Progress, &message, sizeof(message));
    }

    void text(HelperMessage kind, std::string_view message) {
        send(kind, message.data(), std::min<usize>(message.size(), format::kMaxMessagePayload));
    }

private:
    static void writeAll(const void* data, usize size) {
        const auto* cursor = static_cast<const u8*>(data);
        while (size != 0) {
            const ssize_t count = write(STDOUT_FILENO, cursor, size);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                _exit(1); // the viewer has gone
            }
            cursor += count;
            size -= usize(count);
        }
    }

    std::mutex mutex_;
};

std::string siblingPath(const char* executable, std::string_view name) {
    std::string path(executable);
    const usize slash = path.rfind('/');
    path.erase(slash == std::string::npos ? 0 : slash + 1);
    path.append(name);
    return path;
}

int fail(MessageWriter& writer, std::string_view message) {
    writer.text(HelperMessage::Failed, message);
    return 1;
}

} // namespace

int main(int argc, const char** argv) {
    MessageWriter writer;
    if (argc != 3) {
        return fail(writer, "The helper was started incorrectly.");
    }
    const int sourceFd = std::atoi(argv[1]);
    const int outputFd = std::atoi(argv[2]);

    struct stat info{};
    if (fstat(sourceFd, &info) != 0 || info.st_size <= 0) {
        return fail(writer, "This file is empty.");
    }
    const auto size = usize(info.st_size);
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, sourceFd, 0);
    if (mapping == MAP_FAILED) {
        return fail(writer, "This file could not be read.");
    }

    const std::span<const u8> file(static_cast<const u8*>(mapping), size);
    std::string error;
    std::vector<std::string> warnings;
    if (import::looksLikeStep(file)) {
        import::StepOptions options;
        options.workerPath = siblingPath(argv[0], "geometry-occt-worker");
        options.sourceFd = sourceFd;
        import::StepStats stats;
        if (!import::importStep(file, outputFd, &writer, options, stats, error)) {
            return fail(writer, error);
        }
        warnings = std::move(stats.warnings);
    } else {
        import::ImportResult result;
        if (!import::importMesh(file, outputFd, &writer, result, error)) {
            return fail(writer, error);
        }
        warnings = std::move(result.warnings);
    }
    for (const std::string& warning : warnings) {
        writer.text(HelperMessage::Warning, warning);
    }
    writer.send(HelperMessage::Done, nullptr, 0);
    return 0;
}
