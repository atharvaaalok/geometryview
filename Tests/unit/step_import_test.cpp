// Runs whole STEP imports through the real OCCT worker. Skipped when the worker is not built.
#include "format/blob.h"
#include "step/step_import.h"
#include "step_fixture.h"

#include <cmath>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <vector>

using namespace geo;
namespace fs = std::filesystem;

#ifdef GEO_TEST_WORKER_PATH

namespace {

struct Totals {
    usize definitions = 0, occurrences = 0, triangles = 0;
    double area = 0.0, volume = 0.0;
};

// Imports `text` and totals the world-space surface the cache file describes.
bool importText(const std::string& text, const std::string& worker, Totals& totals,
                import::StepStats& stats, std::string& error) {
    char path[] = "/tmp/geostepXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::StepOptions options;
    options.workerPath = worker;
    const bool ok = import::importStep({reinterpret_cast<const u8*>(text.data()), text.size()}, fd,
                                       nullptr, options, stats, error);
    std::vector<u8> bytes(usize(lseek(fd, 0, SEEK_END)));
    pread(fd, bytes.data(), bytes.size(), 0);
    close(fd);
    unlink(path);
    if (!ok) {
        return false;
    }

    std::span<const format::SegmentEntry> segments;
    REQUIRE(format::parseHeader(bytes, segments, error));
    std::vector<format::SegmentView> views(segments.size());
    const format::SegmentView* model = nullptr;
    for (usize i = 0; i < segments.size(); ++i) {
        REQUIRE(format::parseSegment(
            std::span<const u8>(bytes).subspan(segments[i].offset, segments[i].size), views[i],
            error));
        if (views[i].hasModel()) {
            model = &views[i];
        }
    }
    REQUIRE(model != nullptr);
    totals.definitions = model->definitionInfos.size();
    totals.occurrences = model->occurrences.size();
    for (const format::SegmentView& view : views) {
        for (const format::DefinitionRecord& d : view.definitions) {
            u32 placements = 0;
            for (const format::OccurrenceRecord& o : model->occurrences) {
                placements += o.definition == d.definition;
            }
            const auto position = [&](u32 vertex) {
                const PackedVertex& v = view.vertices[d.firstVertex + vertex];
                std::array<u32, 3> grid = {v.x, v.y, v.z};
                if ((d.flags & kDefinitionWide) != 0) {
                    const u32 high = view.highBits[d.firstHighBits + vertex];
                    for (int axis = 0; axis < 3; ++axis) {
                        grid[axis] |= ((high >> (8 * axis)) & 0xFF) << 16;
                    }
                }
                std::array<double, 3> p{};
                for (int axis = 0; axis < 3; ++axis) {
                    p[axis] = double(grid[axis]) * d.decodeScale[axis] + d.boundsMin[axis];
                }
                return p;
            };
            for (u32 m = 0; m < d.meshletCount; ++m) {
                const Meshlet& meshlet = view.meshlets[d.firstMeshlet + m];
                for (u32 t = 0; t < meshlet.triangleCount; ++t) {
                    const u32 packed = view.triangles[d.firstTriangle + meshlet.triangleOffset + t];
                    const auto a = position(meshlet.vertexOffset + (packed & 0xFF));
                    const auto b = position(meshlet.vertexOffset + ((packed >> 8) & 0xFF));
                    const auto c = position(meshlet.vertexOffset + ((packed >> 16) & 0xFF));
                    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                    const double w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                    const double n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2],
                                         u[0] * w[1] - u[1] * w[0]};
                    totals.area +=
                        placements * 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    totals.volume += placements * (a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) / 6.0;
                    totals.triangles += placements;
                }
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("an assembly imports through OCCT workers with shared definitions") {
    test::StepWriter writer;
    const int root = writer.assembly("rig");
    const int a = writer.box({0, 0, 0}, {1, 2, 3});
    const int b = writer.box({0, 0, 0}, {1, 2, 3});
    writer.place(root, writer.part("left", a), a, {10, 0, 0});
    writer.place(root, writer.part("right", b), b, {20, 0, 0});

    Totals totals;
    import::StepStats stats;
    std::string error;
    REQUIRE_MESSAGE(importText(writer.text(), GEO_TEST_WORKER_PATH, totals, stats, error), error);
    CHECK(stats.failedFaces == 0);
    CHECK(totals.definitions == 1);
    CHECK(totals.occurrences == 2);
    CHECK(totals.triangles == 24);
    CHECK(totals.area == doctest::Approx(2 * 22.0).epsilon(1e-4));
    CHECK(totals.volume == doctest::Approx(2 * 6.0).epsilon(1e-4));
}

TEST_CASE("a cavity is subtracted") {
    test::StepWriter writer;
    writer.part("hollow", writer.box({0, 0, 0}, {4, 4, 4}, true));
    Totals totals;
    import::StepStats stats;
    std::string error;
    REQUIRE_MESSAGE(importText(writer.text(), GEO_TEST_WORKER_PATH, totals, stats, error), error);
    CHECK(totals.volume == doctest::Approx(64.0 - 8.0).epsilon(1e-4));
}

TEST_CASE("a crashing worker fails the import without taking the caller down") {
    const fs::path crasher = fs::temp_directory_path() / "geo-crashing-worker.sh";
    std::ofstream(crasher) << "#!/bin/sh\nkill -SEGV $$\n";
    fs::permissions(crasher, fs::perms::owner_all);
    test::StepWriter writer;
    writer.part("box", writer.box({0, 0, 0}, {1, 1, 1}));

    Totals totals;
    import::StepStats stats;
    std::string error;
    CHECK_FALSE(importText(writer.text(), crasher.string(), totals, stats, error));
    CHECK(stats.crashedWorkers > 0);
    CHECK_FALSE(error.empty());
    fs::remove(crasher);
}

#endif
