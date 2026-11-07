// geometry-import: the sandboxed helper that parses source files.
//
// Untrusted files are parsed here, in a process of its own, so a malformed file can only crash this
// process, its memory returns to the system when it exits, and the viewer never links a parser.
//
// Usage: geometry-import <source-fd> <output-fd> [--level <n> --definitions <i,j,...>]. The
// options refine a STEP file: only those definitions, at that level, without the model. Results are
// framed messages on stdout, in format/helper_messages.h. OCCT runs in a further process, next to
// this executable.
#include "format/helper_messages.h"
#include "import.h"

#include <cerrno>
#include <charconv>
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

bool parseList(std::string_view text, std::vector<u32>& values) {
    while (!text.empty()) {
        const usize comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        u32 value = 0;
        const auto [end, status] = std::from_chars(item.data(), item.data() + item.size(), value);
        if (status != std::errc() || end != item.data() + item.size()) {
            return false;
        }
        values.push_back(value);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return !values.empty();
}

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
    if (argc != 3 && argc != 7) {
        return fail(writer, "The helper was started incorrectly.");
    }
    const int sourceFd = std::atoi(argv[1]);
    const int outputFd = std::atoi(argv[2]);
    u32 level = 0;
    std::vector<u32> definitions;
    if (argc == 7) {
        if (std::string_view(argv[3]) != "--level" ||
            std::string_view(argv[5]) != "--definitions" || !parseList(argv[6], definitions)) {
            return fail(writer, "The helper was started incorrectly.");
        }
        level = u32(std::atoi(argv[4]));
    }

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
        options.level = level;
        options.definitions = std::move(definitions);
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
