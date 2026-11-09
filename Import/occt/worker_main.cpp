// geometryview-occt-worker: reads STEP text with OCCT and returns triangles.
//
// OCCT uses gigabytes on large inputs and can crash on bad ones, so it runs only here, in small
// single-threaded processes the supervisor can replace. Each request is one slice of the file.
//
// With --truth it instead reads a file the ordinary way and prints totals, which is how the
// slicing pipeline is checked against OCCT itself.
//
// For comparing builds, GEOMETRYVIEW_DUMP_SLICES=<folder> makes it save every slice it is sent,
// with the request and its own timings, and --replay <folder> meshes saved slices one after another
// and prints what each took. The WebAssembly build runs the same replay.
#include "base/types.h"
#include "occt/mesh_shape.h"
#include "occt/worker_protocol.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
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

// Saving slices for replay, when GEOMETRYVIEW_DUMP_SLICES names a folder.
class SliceDump {
public:
    SliceDump() {
        const char* folder = std::getenv("GEOMETRYVIEW_DUMP_SLICES");
        if (folder != nullptr && *folder != '\0') {
            folder_ = folder;
            prefix_ = std::to_string(getpid());
            index_ = std::fopen((folder_ + "/" + prefix_ + ".jsonl").c_str(), "a");
        }
    }
    ~SliceDump() {
        if (index_ != nullptr) {
            std::fclose(index_);
        }
    }
    void save(std::string_view text, const Request& request, const Reply& reply) {
        if (index_ == nullptr) {
            return;
        }
        const std::string name = prefix_ + "-" + std::to_string(count_++) + ".step";
        if (FILE* file = std::fopen((folder_ + "/" + name).c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
        std::println(index_,
                     "{{\"file\": \"{}\", \"bytes\": {}, \"linear\": {}, \"angular\": {}, "
                     "\"minSize\": {}, \"status\": {}, \"faces\": {}, \"unmeshed\": {}, "
                     "\"payloadBytes\": {}, \"read\": {}, \"transfer\": {}, \"mesh\": {}}}",
                     name, text.size(), request.linearDeflection, request.angularDeflection,
                     request.minSize, reply.status, reply.faceCount, reply.unmeshedFaces,
                     reply.payloadBytes, reply.readSeconds, reply.transferSeconds,
                     reply.meshSeconds);
        std::fflush(index_);
    }

private:
    std::string folder_, prefix_;
    FILE* index_ = nullptr;
    u32 count_ = 0;
};

int serve() {
    SliceDump dump;
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
            text.resize(usize(request.textBytes)); // a slice is far below 4 GB
            if (!readExactly(kRequestFd, text.data(), text.size())) {
                return 0;
            }
            meshSlice(text, request, reply, payload);
            reply.payloadBytes = payload.size();
            dump.save(text, request, reply);
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

// A number field of one line of a dump's index, which this tool wrote itself.
double field(std::string_view line, std::string_view name) {
    const std::string key = "\"" + std::string(name) + "\": ";
    const usize at = line.find(key);
    return at == std::string_view::npos ? 0.0 : std::atof(line.data() + at + key.size());
}

std::string textField(std::string_view line, std::string_view name) {
    const std::string key = "\"" + std::string(name) + "\": \"";
    const usize at = line.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const usize start = at + key.size();
    return std::string(line.substr(start, line.find('"', start) - start));
}

// The area and signed volume of a reply's triangles, to compare tessellations that need not match
// triangle for triangle.
void measurePayload(const std::vector<u8>& payload, u32 faces, double& area, double& volume,
                    u64& triangles) {
    area = volume = 0.0;
    triangles = 0;
    usize at = 0;
    for (u32 f = 0; f < faces && at + sizeof(FaceHeader) <= payload.size(); ++f) {
        FaceHeader header{};
        std::memcpy(&header, payload.data() + at, sizeof(header));
        at += sizeof(header);
        const usize positionsAt = at;
        at += usize(header.vertexCount) * 6 * sizeof(float);
        std::vector<float> positions(usize(header.vertexCount) * 3);
        std::memcpy(positions.data(), payload.data() + positionsAt, positions.size() * sizeof(float));
        for (u32 t = 0; t < header.triangleCount; ++t) {
            u32 index[3];
            std::memcpy(index, payload.data() + at + usize(t) * 12, 12);
            double p[3][3];
            for (u32 k = 0; k < 3; ++k) {
                for (u32 axis = 0; axis < 3; ++axis) {
                    p[k][axis] = positions[usize(index[k]) * 3 + axis];
                }
            }
            const double u[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const double v[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                 u[0] * v[1] - u[1] * v[0]};
            area += 0.5 * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            volume += (p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) -
                       p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                       p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0])) /
                      6.0;
        }
        at += usize(header.triangleCount) * 12;
        triangles += header.triangleCount;
    }
}

// Meshes saved slices one after another and prints what each took, as JSON lines.
int replay(int argc, const char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: --replay <folder> [limit]");
        return 2;
    }
    const std::string folder = argv[2];
    const usize limit = argc > 3 ? usize(std::atoll(argv[3])) : usize(-1);
    std::vector<std::string> indexes;
    if (DIR* directory = opendir(folder.c_str())) {
        while (const dirent* entry = readdir(directory)) {
            const std::string_view name = entry->d_name;
            if (name.ends_with(".jsonl")) {
                indexes.emplace_back(name);
            }
        }
        closedir(directory);
    }
    std::sort(indexes.begin(), indexes.end());
    std::vector<u8> payload;
    usize done = 0;
    for (const std::string& index : indexes) {
        std::ifstream lines(folder + "/" + index);
        for (std::string line; done < limit && std::getline(lines, line);) {
            const std::string name = textField(line, "file");
            std::ifstream file(folder + "/" + name, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            Request request{};
            request.magic = kMagic;
            request.kind = u32(RequestKind::Slice);
            request.textBytes = text.size();
            request.linearDeflection = field(line, "linear");
            request.angularDeflection = field(line, "angular");
            request.minSize = field(line, "minSize");
            Reply reply{};
            payload.clear();
            meshSlice(text, request, reply, payload);
            double area = 0.0, volume = 0.0;
            u64 triangles = 0;
            measurePayload(payload, reply.faceCount, area, volume, triangles);
            std::println("{{\"file\": \"{}\", \"bytes\": {}, \"status\": {}, \"faces\": {}, "
                         "\"unmeshed\": {}, \"payloadBytes\": {}, \"triangles\": {}, "
                         "\"area\": {:.9g}, \"volume\": {:.9g}, \"read\": {:.6f}, "
                         "\"transfer\": {:.6f}, \"mesh\": {:.6f}}}",
                         name, text.size(), reply.status, reply.faceCount, reply.unmeshedFaces,
                         payload.size(), triangles, area, volume, reply.readSeconds,
                         reply.transferSeconds, reply.meshSeconds);
            ++done;
        }
    }
    return 0;
}

} // namespace

int main(int argc, const char** argv) {
    initializeOcct();
    if (argc >= 2 && std::string_view(argv[1]) == "--truth") {
        return truth(argc, argv);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--replay") {
        return replay(argc, argv);
    }
    return serve();
}
