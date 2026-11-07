#include "mesh/packer.h"

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <map>

using namespace geo;
using namespace geo::import;

namespace {

// A wavy grid with `size` squares per side, so meshlets have real connectivity.
TriangleSoup grid(int size, float span) {
    TriangleSoup soup;
    const auto point = [&](int x, int y) {
        const float px = float(x) / float(size), py = float(y) / float(size);
        return std::array<float, 3>{px * span, py * span, 0.05f * span * std::sin(py * 9.0f)};
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto a = point(x, y), b = point(x + 1, y), c = point(x + 1, y + 1),
                       d = point(x, y + 1);
            for (const auto& p : {a, b, c, a, c, d}) {
                soup.corners.insert(soup.corners.end(), p.begin(), p.end());
            }
        }
    }
    return soup;
}

} // namespace

TEST_CASE("packing keeps every triangle within meshlet limits") {
    const IndexedMesh mesh = buildIndexedMesh(grid(64, 10.0f));
    const PackedDefinition packed = packMesh(mesh);

    usize triangles = 0;
    for (const Meshlet& m : packed.meshlets) {
        CHECK(m.vertexCount <= kMeshletMaxVertices);
        CHECK(m.triangleCount <= kMeshletMaxTriangles);
        CHECK(m.triangleCount > 0);
        triangles += m.triangleCount;
    }
    CHECK(triangles == mesh.triangleCount());
    CHECK(packed.triangles.size() == mesh.triangleCount());
}

TEST_CASE("packed positions decode to within one grid step") {
    const IndexedMesh mesh = buildIndexedMesh(grid(32, 10.0f));
    const PackedDefinition packed = packMesh(mesh);

    const float step = std::sqrt(packed.decodeScale[0] * packed.decodeScale[0] +
                                 packed.decodeScale[1] * packed.decodeScale[1] +
                                 packed.decodeScale[2] * packed.decodeScale[2]);
    for (usize v = 0; v < packed.vertices.size(); ++v) {
        const std::array<float, 3> p = decodePosition(packed, v);
        float nearest = 1e30f;
        for (usize o = 0; o < mesh.vertexCount(); ++o) {
            const float dx = p[0] - mesh.positions[o * 3];
            const float dy = p[1] - mesh.positions[o * 3 + 1];
            const float dz = p[2] - mesh.positions[o * 3 + 2];
            nearest = std::min(nearest, dx * dx + dy * dy + dz * dz);
        }
        CHECK(std::sqrt(nearest) <= step);
    }
}

TEST_CASE("each meshlet's local faces map back to the input's faces") {
    TriangleSoup soup = grid(20, 1.0f);
    TriangleSoup second = grid(20, 1.0f);
    for (usize i = 2; i < second.corners.size(); i += 3) {
        second.corners[i] += 7.0f;
    }
    soup.corners.insert(soup.corners.end(), second.corners.begin(), second.corners.end());
    const IndexedMesh mesh = buildIndexedMesh(soup);
    REQUIRE(mesh.faceCount == 2);
    const PackedDefinition packed = packMesh(mesh);

    std::map<u32, usize> perFace;
    for (const Meshlet& m : packed.meshlets) {
        for (u32 t = 0; t < m.triangleCount; ++t) {
            const u32 encoded = packed.triangles[m.triangleOffset + t];
            const u32 a = encoded & 0xFF;
            CHECK(a < m.vertexCount);
            const PackedVertex& v = packed.vertices[m.vertexOffset + a];
            ++perFace[packed.originalFace[m.faceBase + v.localFace]];
        }
    }
    CHECK(perFace[0] == 800);
    CHECK(perFace[1] == 800);
}

TEST_CASE("a large mesh with very fine edges gets the wide grid") {
    // 512 squares across 4000 units gives edges of about 8 units, far above the 16-bit step.
    CHECK((packMesh(buildIndexedMesh(grid(512, 4000.0f))).flags & kDefinitionWide) == 0);

    // One small detail stretched across a huge extent falls below eight 16-bit grid steps.
    TriangleSoup soup = grid(64, 0.01f);
    const std::array<float, 9> far = {100000, 0, 0, 100001, 0, 0, 100000, 1, 0};
    soup.corners.insert(soup.corners.end(), far.begin(), far.end());
    const IndexedMesh mesh = buildIndexedMesh(soup);
    const PackedDefinition packed = packMesh(mesh);
    CHECK((packed.flags & kDefinitionWide) != 0);
    CHECK(packed.highBits.size() == packed.vertices.size());

    // The 24-bit grid keeps the far corner's position to about 0.012 units.
    float nearest = 1e30f;
    for (usize v = 0; v < packed.vertices.size(); ++v) {
        const std::array<float, 3> p = decodePosition(packed, v);
        nearest = std::min(nearest, std::fabs(p[0] - 100001.0f) + std::fabs(p[1]));
    }
    CHECK(nearest < 0.05f);
}

TEST_CASE("meshlets are well filled on connected surfaces") {
    const PackedDefinition packed = packMesh(buildIndexedMesh(grid(128, 10.0f)));
    usize triangles = 0;
    for (const Meshlet& m : packed.meshlets) {
        triangles += m.triangleCount;
    }
    const double fill = double(triangles) / double(packed.meshlets.size() * kMeshletMaxTriangles);
    CHECK(fill > 0.8);
}

TEST_CASE("bounds contain every vertex of their meshlet") {
    const PackedDefinition packed = packMesh(buildIndexedMesh(grid(64, 10.0f)));
    for (const Meshlet& m : packed.meshlets) {
        const std::array<float, 3> center = {
            float(m.centerX) * packed.boundsScale[0] + packed.boundsMin[0],
            float(m.centerY) * packed.boundsScale[1] + packed.boundsMin[1],
            float(m.centerZ) * packed.boundsScale[2] + packed.boundsMin[2]};
        const float step =
            std::max({packed.boundsScale[0], packed.boundsScale[1], packed.boundsScale[2]});
        const float radius = float(m.radius) * step;
        for (u32 v = 0; v < m.vertexCount; ++v) {
            const std::array<float, 3> p = decodePosition(packed, m.vertexOffset + v);
            const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
            CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) <= radius * 1.0001f);
        }
    }
}

TEST_CASE("packing an empty mesh gives an empty definition") {
    const PackedDefinition packed = packMesh(IndexedMesh{});
    CHECK(packed.meshlets.empty());
}

TEST_CASE("a vertex at the top of the wide grid stays there") {
    // Tiny triangles force the wide grid, and the large span puts a vertex at its very top.
    import::TriangleSoup soup;
    const float size = 123456.7f;
    for (float x = 0.0f; x < 40.0f; x += 1.0f) {
        soup.corners.insert(soup.corners.end(), {x, 0, 0, x + 0.25f, 0, 0, x, 0.25f, 0});
    }
    soup.corners.insert(soup.corners.end(),
                        {size, size, size, size - 0.25f, size, size, size, size - 0.25f, size});
    const import::PackedDefinition packed = import::packMesh(import::buildIndexedMesh(soup));
    REQUIRE((packed.flags & kDefinitionWide) != 0);
    float largest = 0.0f;
    for (usize v = 0; v < packed.vertices.size(); ++v) {
        largest = std::max(largest, import::decodePosition(packed, v)[0]);
    }
    CHECK(largest == doctest::Approx(size).epsilon(1e-5));
}
