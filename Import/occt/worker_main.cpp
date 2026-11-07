// geometry-occt-worker: reads STEP text with OCCT and returns triangles.
//
// OCCT uses gigabytes on large inputs and can crash on bad ones, so it runs only here, in small
// single-threaded processes the supervisor can replace. Each request is one slice of the file.
//
// With --truth it instead reads a file the ordinary way and prints totals, which is how the
// slicing pipeline is checked against OCCT itself.
#include "base/types.h"
#include "occt/mesh_shape.h"
#include "occt/worker_protocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace geo;
using namespace geo::import::worker;

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

bool writeExactly(int fd, const void* data, usize size) {
    const auto* cursor = static_cast<const u8*>(data);
    while (size != 0) {
        const ssize_t count = write(fd, cursor, size);
        if (count >= 0) {
            cursor += count;
            size -= usize(count);
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

int serve() {
    std::string text;
    std::vector<u8> payload;
    while (true) {
        Request request{};
        if (!readExactly(kRequestFd, &request, sizeof(request))) {
            return 0; // the supervisor has finished, or has gone
        }
        if (request.magic != kMagic) {
            return 1;
        }
        Reply reply{};
        reply.magic = kMagic;
        payload.clear();
        if (RequestKind(request.kind) == RequestKind::WholeFile) {
            struct stat info{};
            if (fstat(kSourceFd, &info) != 0) {
                return 1;
            }
            const auto size = usize(info.st_size);
            void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, kSourceFd, 0);
            if (mapping == MAP_FAILED) {
                return 1;
            }
            meshWholeFile({static_cast<const char*>(mapping), size}, request, reply, payload);
            munmap(mapping, size);
        } else {
            text.resize(request.textBytes);
            if (!readExactly(kRequestFd, text.data(), text.size())) {
                return 0;
            }
            meshSlice(text, request, reply, payload);
        }
        reply.payloadBytes = payload.size();
        if (!writeExactly(kReplyFd, &reply, sizeof(reply)) ||
            !writeExactly(kReplyFd, payload.data(), payload.size())) {
            return 1;
        }
    }
}

int truth(int argc, const char** argv) {
    if (argc != 6) {
        std::println(stderr, "usage: --truth <file> <linear> <angular> <min-size>");
        return 2;
    }
    const Truth result =
        measureFile(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]));
    std::println("{{\"ok\": {}, \"faces\": {}, \"unmeshed\": {}, \"triangles\": {}, "
                 "\"area\": {}, \"signedVolume\": {}, \"min\": [{}, {}, {}], "
                 "\"max\": [{}, {}, {}]}}",
                 result.ok, result.faces, result.unmeshedFaces, result.triangles, result.area,
                 result.signedVolume, result.min[0], result.min[1], result.min[2], result.max[0],
                 result.max[1], result.max[2]);
    return result.ok ? 0 : 1;
}

} // namespace

int main(int argc, const char** argv) {
    initializeOcct();
    if (argc >= 2 && std::string_view(argv[1]) == "--truth") {
        return truth(argc, argv);
    }
    return serve();
}
