// gcli: the headless command-line front end used for rendering, benchmarks and tests.
#include "base/time.h"
#include "engine.h"
#include "format/blob.h"
#include "import.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace geo;

struct MappedFile {
    const u8* data = nullptr;
    usize size = 0;
    int fd = -1;

    ~MappedFile() {
        if (data) {
            munmap(const_cast<u8*>(data), size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    bool open(const char* path) {
        fd = ::open(path, O_RDONLY);
        struct stat info{};
        if (fd < 0 || fstat(fd, &info) != 0 || info.st_size == 0) {
            return false;
        }
        size = usize(info.st_size);
        void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            return false;
        }
        data = static_cast<const u8*>(mapping);
        return true;
    }
};

int importStepCommand(std::span<const u8> bytes, int sourceFd, int outputFd, const char* worker,
                      const char* levelText) {
    import::StepOptions options;
    options.workerPath = worker;
    options.sourceFd = sourceFd;
    options.level = levelText != nullptr ? u32(std::atoi(levelText)) : 0;
    import::StepStats s;
    std::string error;
    const double start = nowSeconds();
    if (!import::importStep(bytes, outputFd, nullptr, options, s, error)) {
        std::println(stderr, "{}", error);
        return 1;
    }
    const double total = nowSeconds() - start;
    const off_t blobSize = lseek(outputFd, 0, SEEK_END);
    std::println("{:.1f} MB source, {} records, {} bodies, {} definitions, {} occurrences{}",
                 double(bytes.size()) / 1e6, s.records, s.bodies, s.definitions, s.occurrences,
                 s.wholeFile ? ", whole-file read" : "");
    std::println("{} faces in {} slices, {} workers, {} triangles, {} meshlets", s.faces, s.chunks,
                 s.workers, s.triangles, s.meshlets);
    std::println("tolerances: linear {} angular {} min {}", s.linearDeflection, s.angularDeflection,
                 s.minSize);
    std::println("failed {} unmeshed {} unmapped {} crashed {}", s.failedFaces, s.unmeshedFaces,
                 s.unmappedFaces, s.crashedWorkers);
    std::println("tokenize {:.1f} ms, structure {:.1f} ms, definitions {:.1f} ms, workers {:.3f} s",
                 s.tokenizeSeconds * 1e3, s.structureSeconds * 1e3, s.definitionSeconds * 1e3,
                 s.workerSeconds);
    std::println("cpu: read {:.3f} s, transfer {:.3f} s, mesh {:.3f} s, pack {:.3f} s",
                 s.readSeconds, s.transferSeconds, s.meshSeconds, s.packSeconds);
    std::println("total {:.3f} s, blob {:.2f} MB, {:.1f} bytes per triangle", total,
                 double(blobSize) / 1e6,
                 double(blobSize) / double(std::max<usize>(s.triangles, 1)));
    for (const std::string& warning : s.warnings) {
        std::println("warning: {}", warning);
    }
    return 0;
}

int importCommand(const char* input, const char* output, const char* worker,
                  const char* levelText) {
    MappedFile file;
    if (!file.open(input)) {
        std::println(stderr, "could not read {}: {}", input, std::strerror(errno));
        return 1;
    }
    const int fd = open(output, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::println(stderr, "could not create {}: {}", output, std::strerror(errno));
        return 1;
    }

    const std::span<const u8> bytes(file.data, file.size);
    if (import::looksLikeStep(bytes)) {
        const int result = importStepCommand(bytes, file.fd, fd, worker, levelText);
        close(fd);
        return result;
    }

    import::ImportResult result;
    std::string error;
    const double start = nowSeconds();
    const bool ok = import::importMesh(bytes, fd, nullptr, result, error);
    const double total = nowSeconds() - start;
    const off_t blobSize = lseek(fd, 0, SEEK_END);
    close(fd);
    if (!ok) {
        std::println(stderr, "{}", error);
        return 1;
    }

    std::println("{:.1f} MB source, {} triangles, {} faces, {} meshlets, {} grid",
                 double(file.size) / 1e6, result.triangles, result.faces, result.meshlets,
                 result.wide ? "24-bit" : "16-bit");
    std::println("parse {:.3f} s, regions {:.3f} s, pack {:.3f} s, write {:.3f} s, total {:.3f} s",
                 result.times.parse, result.times.regions, result.times.pack, result.times.write,
                 total);
    std::println("blob {:.1f} MB, {:.1f} bytes per triangle", double(blobSize) / 1e6,
                 double(blobSize) / double(result.triangles));
    for (const std::string& warning : result.warnings) {
        std::println("warning: {}", warning);
    }
    return 0;
}

int verifyCommand(const char* path) {
    MappedFile file;
    if (!file.open(path)) {
        std::println(stderr, "could not read {}", path);
        return 1;
    }
    std::span<const format::SegmentEntry> segments;
    std::string error;
    const double start = nowSeconds();
    if (!format::parseHeader({file.data, file.size}, segments, error)) {
        std::println(stderr, "invalid: {}", error);
        return 1;
    }
    usize meshlets = 0, vertices = 0, triangles = 0, definitions = 0, occurrences = 0;
    for (const format::SegmentEntry& entry : segments) {
        format::SegmentView view;
        if (!format::parseSegment({file.data + entry.offset, entry.size}, view, error)) {
            std::println(stderr, "invalid: {}", error);
            return 1;
        }
        meshlets += view.meshlets.size();
        vertices += view.vertices.size();
        triangles += view.triangles.size();
        definitions += view.definitions.size();
        occurrences += view.occurrences.size();
    }
    std::println("valid: {} segments, {} definitions, {} occurrences, {} meshlets, {} vertices, "
                 "{} triangles, checked in {:.2f} ms",
                 segments.size(), definitions, occurrences, meshlets, vertices, triangles,
                 (nowSeconds() - start) * 1e3);
    return 0;
}

int renderCommand(int argc, const char** argv) {
    // gcli render <metallib> <blob> <png> [width height azimuth elevation perspective]
    const char* library = argv[2];
    const char* blob = argv[3];
    const char* png = argv[4];
    const u32 width = argc > 5 ? u32(std::atoi(argv[5])) : 1600;
    const u32 height = argc > 6 ? u32(std::atoi(argv[6])) : 1000;
    const double azimuth = argc > 7 ? std::atof(argv[7]) : 35.0;
    const double elevation = argc > 8 ? std::atof(argv[8]) : 25.0;
    const bool perspective = argc > 9 && std::string_view(argv[9]) == "perspective";
    return geo_render_blob_to_png(library, blob, png, width, height, azimuth, elevation,
                                  perspective)
               ? 0
               : 1;
}

} // namespace

int main(int argc, const char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "import" && argc >= 4) {
        return importCommand(argv[2], argv[3], argc > 4 ? argv[4] : "",
                             argc > 5 ? argv[5] : nullptr);
    }
    if (command == "verify" && argc == 3) {
        return verifyCommand(argv[2]);
    }
    if (command == "render" && argc >= 5) {
        return renderCommand(argc, argv);
    }
    std::println(stderr, "gcli {}", geo_engine_version());
    std::println(stderr, "usage: gcli import <source> <blob> [occt-worker [level]]");
    std::println(stderr, "       gcli verify <blob>");
    std::println(stderr, "       gcli render <metallib> <blob> <png> "
                         "[width height azimuth elevation perspective]");
    return 2;
}
