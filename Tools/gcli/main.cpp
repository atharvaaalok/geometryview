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

int importCommand(const char* input, const char* output) {
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

    import::ImportResult result;
    std::string error;
    const double start = nowSeconds();
    const bool ok = import::importMesh({file.data, file.size}, fd, result, error);
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
    format::BlobView view;
    std::string error;
    const double start = nowSeconds();
    if (!format::parseBlob({file.data, file.size}, view, error)) {
        std::println(stderr, "invalid: {}", error);
        return 1;
    }
    std::println("valid: {} meshlets, {} vertices, {} triangles, checked in {:.2f} ms",
                 view.meshlets.size(), view.vertices.size(), view.triangles.size(),
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
    return geo_render_blob_to_png(library, blob, png, width, height, azimuth, elevation, perspective)
               ? 0
               : 1;
}

} // namespace

int main(int argc, const char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "import" && argc == 4) {
        return importCommand(argv[2], argv[3]);
    }
    if (command == "verify" && argc == 3) {
        return verifyCommand(argv[2]);
    }
    if (command == "render" && argc >= 5) {
        return renderCommand(argc, argv);
    }
    std::println(stderr, "gcli {}", geo_engine_version());
    std::println(stderr, "usage: gcli import <source> <blob>");
    std::println(stderr, "       gcli verify <blob>");
    std::println(stderr, "       gcli render <metallib> <blob> <png> "
                         "[width height azimuth elevation perspective]");
    return 2;
}
