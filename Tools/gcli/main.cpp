// gcli: the headless command-line front end used for rendering, benchmarks and tests.
#include "base/time.h"
#include "engine.h"
#include "format/blob.h"
#include "import.h"
#include "sheet.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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
    std::println("failed {} unmapped {} crashed {} coarser {}", s.failedFaces, s.unmappedFaces,
                 s.crashedWorkers, s.coarserFaces);
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

struct Moments {
    double area = 0.0;
    double signedVolume = 0.0;
};

std::array<float, 3> decodeVertex(const format::SegmentView& view,
                                  const format::DefinitionRecord& d, u32 vertex) {
    const PackedVertex& v = view.vertices[d.firstVertex + vertex];
    std::array<u32, 3> grid = {v.x, v.y, v.z};
    if ((d.flags & kDefinitionWide) != 0) {
        const u32 high = view.highBits[d.firstHighBits + vertex];
        grid[0] |= (high & 0xFF) << 16;
        grid[1] |= ((high >> 8) & 0xFF) << 16;
        grid[2] |= ((high >> 16) & 0xFF) << 16;
    }
    return {float(grid[0]) * d.decodeScale[0] + d.boundsMin[0],
            float(grid[1]) * d.decodeScale[1] + d.boundsMin[1],
            float(grid[2]) * d.decodeScale[2] + d.boundsMin[2]};
}

// Prints totals in world space, in the same form as the worker's --truth output.
int statsCommand(const char* path) {
    MappedFile file;
    if (!file.open(path)) {
        std::println(stderr, "could not read {}", path);
        return 1;
    }
    std::span<const format::SegmentEntry> segments;
    std::string error;
    if (!format::parseHeader({file.data, file.size}, segments, error)) {
        std::println(stderr, "invalid: {}", error);
        return 1;
    }
    std::vector<format::SegmentView> views(segments.size());
    const format::SegmentView* model = nullptr;
    for (usize i = 0; i < segments.size(); ++i) {
        if (!format::parseSegment({file.data + segments[i].offset, segments[i].size}, views[i],
                                  error)) {
            std::println(stderr, "invalid: {}", error);
            return 1;
        }
        if (views[i].hasModel()) {
            model = &views[i];
        }
    }
    if (model == nullptr) {
        std::println(stderr, "no model");
        return 1;
    }

    struct Geometry {
        const format::SegmentView* view = nullptr;
        const format::DefinitionRecord* record = nullptr;
    };
    std::vector<Geometry> geometry(model->definitionInfos.size());
    for (const format::SegmentView& view : views) {
        for (const format::DefinitionRecord& record : view.definitions) {
            geometry[record.definition] = {&view, &record};
        }
    }

    double area = 0.0, volume = 0.0;
    u64 triangles = 0, faces = 0;
    std::array<u64, 3> edges{}; // segments by class, counting every occurrence
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const format::OccurrenceRecord& occurrence : model->occurrences) {
        faces += model->definitionInfos[occurrence.definition].faceCount;
        const Geometry& g = geometry[occurrence.definition];
        if (g.record == nullptr) {
            continue;
        }
        const format::DefinitionRecord& d = *g.record;
        for (u32 s = 0; s < d.edgeSegmentCount; ++s) {
            const u32 edgeClass =
                g.view->edgeSegments[d.firstEdgeSegment + s].faceB >> kEdgeClassShift;
            ++edges[std::min<u32>(edgeClass, 2)];
        }
        const std::array<float, 12>& t = occurrence.transform;
        const auto toWorld = [&](u32 vertex) {
            const std::array<float, 3> p = decodeVertex(*g.view, d, vertex);
            const double x = p[0], y = p[1], z = p[2];
            return std::array<double, 3>{t[0] * x + t[3] * y + t[6] * z + t[9],
                                         t[1] * x + t[4] * y + t[7] * z + t[10],
                                         t[2] * x + t[5] * y + t[8] * z + t[11]};
        };
        for (u32 v = 0; v < d.vertexCount; ++v) {
            const std::array<double, 3> p = toWorld(v);
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = std::min(lo[axis], p[axis]);
                hi[axis] = std::max(hi[axis], p[axis]);
            }
        }
        for (u32 m = 0; m < d.meshletCount; ++m) {
            const Meshlet& meshlet = g.view->meshlets[d.firstMeshlet + m];
            for (u32 i = 0; i < meshlet.triangleCount; ++i) {
                const u32 packed = g.view->triangles[d.firstTriangle + meshlet.triangleOffset + i];
                const std::array<double, 3> a = toWorld(meshlet.vertexOffset + (packed & 0xFF));
                const std::array<double, 3> b =
                    toWorld(meshlet.vertexOffset + ((packed >> 8) & 0xFF));
                const std::array<double, 3> c =
                    toWorld(meshlet.vertexOffset + ((packed >> 16) & 0xFF));
                const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                area += 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
                volume += (a[0] * nx + a[1] * ny + a[2] * nz) / 6.0;
                ++triangles;
            }
        }
    }
    std::println("{{\"faces\": {}, \"triangles\": {}, \"area\": {}, \"signedVolume\": {}, "
                 "\"min\": [{}, {}, {}], \"max\": [{}, {}, {}], "
                 "\"edges\": {{\"crease\": {}, \"smooth\": {}, \"boundary\": {}}}}}",
                 faces, triangles, area, volume, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                 edges[kEdgeCrease], edges[kEdgeSmooth], edges[kEdgeBoundary]);
    return 0;
}

// Prints a string as a JSON string literal.
void printJsonString(std::string_view text) {
    std::print("\"");
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            std::print("\\{}", c);
        } else if (u8(c) < 0x20) {
            std::print("\\u{:04x}", unsigned(u8(c)));
        } else {
            std::print("{}", c);
        }
    }
    std::print("\"");
}

// The assembly tree as JSON: one object per node in depth-first order, with its parent's index,
// its kind, its name and the range of bodies (occurrences) under it.
int treeCommand(const char* path) {
    MappedFile file;
    std::span<const format::SegmentEntry> segments;
    std::string error;
    if (!file.open(path) || !format::parseHeader({file.data, file.size}, segments, error)) {
        std::println(stderr, "could not read {}: {}", path, error);
        return 1;
    }
    for (const format::SegmentEntry& entry : segments) {
        format::SegmentView view;
        if (!format::parseSegment({file.data + entry.offset, entry.size}, view, error)) {
            std::println(stderr, "invalid: {}", error);
            return 1;
        }
        if (!view.hasModel()) {
            continue;
        }
        static constexpr std::array<std::string_view, 3> kinds = {"assembly", "part", "body"};
        std::print("[");
        for (usize n = 0; n < view.nodes.size(); ++n) {
            const format::NodeRecord& node = view.nodes[n];
            std::print("{}{{\"parent\": {}, \"kind\": \"{}\", \"name\": ", n == 0 ? "" : ",\n",
                       node.parent == format::kNoParent ? -1 : i64(node.parent),
                       kinds[std::min<u32>(node.kind, 2)]);
            printJsonString({view.strings.data() + node.nameOffset, node.nameLength});
            std::print(", \"bodies\": [{}, {}]}}", node.firstOccurrence, node.occurrenceEnd);
        }
        std::println("]");
        return 0;
    }
    std::println(stderr, "no model");
    return 1;
}

int renderCommand(int argc, const char** argv) {
    // gcli render <metallib> <blob> <png> [width height azimuth elevation projection style colour
    //                                      section plain]
    // Styles and colour modes are numbered as in engine.h.
    GeoRenderOptions options = geo_default_render_options();
    if (argc > 5) {
        options.width = u32(std::atoi(argv[5]));
    }
    if (argc > 6) {
        options.height = u32(std::atoi(argv[6]));
    }
    if (argc > 7) {
        options.azimuth_degrees = std::atof(argv[7]);
    }
    if (argc > 8) {
        options.elevation_degrees = std::atof(argv[8]);
    }
    options.perspective = argc > 9 && std::string_view(argv[9]) == "perspective";
    if (argc > 10) {
        options.look.style = GeoStyle(std::atoi(argv[10]));
        options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
    }
    if (argc > 11) {
        options.look.color_mode = GeoColorMode(std::atoi(argv[11]));
    }
    // A section as axis letters and fractions, such as "x0.5" or "x0.5,z-0.3", where a minus
    // removes the low side.
    if (argc > 12) {
        for (std::string_view cut :
             std::views::split(std::string_view(argv[12]), ',') | std::views::transform([](auto r) {
                 return std::string_view(r.begin(), r.end());
             })) {
            if (cut.size() < 2 || cut[0] < 'x' || cut[0] > 'z') {
                continue;
            }
            const int axis = cut[0] - 'x';
            const bool flip = cut[1] == '-';
            options.section.enabled[axis] = true;
            options.section.flip[axis] = flip;
            options.section.position[axis] =
                std::strtof(std::string(cut.substr(flip ? 2 : 1)).c_str(), nullptr);
        }
    }
    options.overlay = !(argc > 13 && std::string_view(argv[13]) == "plain");
    // Comma-separated part names to draw alone.
    if (argc > 14) {
        options.isolate = argv[14];
    }
    return geo_render_blob_to_png(argv[2], argv[3], argv[4], &options) ? 0 : 1;
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
    if (command == "stats" && argc == 3) {
        return statsCommand(argv[2]);
    }
    if (command == "render" && argc >= 5) {
        return renderCommand(argc, argv);
    }
    if (command == "tree" && argc == 3) {
        return treeCommand(argv[2]);
    }
    if (command == "sheet") {
        return gcli::sheetCommand(argc, argv);
    }
    std::println(stderr, "gcli {}", geo_engine_version());
    std::println(stderr, "usage: gcli import <source> <blob> [occt-worker [level]]");
    std::println(stderr, "       gcli verify <blob>");
    std::println(stderr, "       gcli stats <blob>");
    std::println(stderr, "       gcli tree <blob>");
    std::println(stderr, "       gcli render <metallib> <blob> <png> [width height azimuth "
                         "elevation ortho|perspective style colour section plain|overlay "
                         "isolate]");
    std::println(stderr,
                 "       gcli sheet <metallib> <png> [--cell 192] [--columns N] [--style S] "
                 "[--colour C] [--same-scale] [--title text] < tiles.jsonl");
    return 2;
}
