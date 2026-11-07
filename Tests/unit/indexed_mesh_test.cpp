#include "mesh/indexed_mesh.h"

#include <array>

#include <cmath>
#include <doctest/doctest.h>
#include <set>

using namespace geo;
using namespace geo::import;

namespace {

void addQuad(std::vector<float>& corners, std::array<float, 3> a, std::array<float, 3> b,
             std::array<float, 3> c, std::array<float, 3> d) {
    for (const auto& p : {a, b, c, a, c, d}) {
        corners.insert(corners.end(), p.begin(), p.end());
    }
}

TriangleSoup cube() {
    TriangleSoup soup;
    auto& c = soup.corners;
    addQuad(c, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}); // bottom
    addQuad(c, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}); // top
    addQuad(c, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}); // front
    addQuad(c, {0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}); // back
    addQuad(c, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}); // left
    addQuad(c, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}); // right
    return soup;
}

// A closed cylinder side with `segments` facets, each split into two triangles.
TriangleSoup tube(int segments) {
    TriangleSoup soup;
    const float step = 6.2831853f / float(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = step * float(i), b = step * float((i + 1) % segments);
        const std::array<float, 3> p0{std::cos(a), std::sin(a), 0}, p1{std::cos(b), std::sin(b), 0};
        const std::array<float, 3> p2{std::cos(b), std::sin(b), 1}, p3{std::cos(a), std::sin(a), 1};
        addQuad(soup.corners, p0, p1, p2, p3);
    }
    return soup;
}

} // namespace

TEST_CASE("a cube has six faces and keeps its creases sharp") {
    const IndexedMesh mesh = buildIndexedMesh(cube());
    CHECK(mesh.faceCount == 6);
    CHECK(mesh.triangleCount() == 12);
    CHECK(mesh.vertexCount() == 24);
    for (usize v = 0; v < mesh.vertexCount(); ++v) {
        const float* n = mesh.normals.data() + v * 3;
        CHECK(std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]) == doctest::Approx(1.0f));
    }
}

TEST_CASE("a finely faceted cylinder is one smooth face") {
    const IndexedMesh mesh = buildIndexedMesh(tube(64));
    CHECK(mesh.faceCount == 1);
    CHECK(mesh.vertexCount() == 128);
    for (usize v = 0; v < mesh.vertexCount(); ++v) {
        const float* p = mesh.positions.data() + v * 3;
        const float* n = mesh.normals.data() + v * 3;
        CHECK(n[0] == doctest::Approx(p[0]).epsilon(0.01));
        CHECK(n[1] == doctest::Approx(p[1]).epsilon(0.01));
        CHECK(n[2] == doctest::Approx(0.0f).epsilon(0.01));
    }
}

TEST_CASE("a coarse cylinder stays faceted") {
    CHECK(buildIndexedMesh(tube(6)).faceCount == 6);
}

TEST_CASE("positive and negative zero weld together") {
    TriangleSoup soup;
    soup.corners = {0.0f, 0, 0, 1, 0, 0, 0, 1, 0, -0.0f, 0, 0, 0, 1, 0, -1, 0, 0};
    const IndexedMesh mesh = buildIndexedMesh(soup);
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount == 1);
}

TEST_CASE("triangles that collapse to a line are removed") {
    TriangleSoup soup;
    soup.corners = {0, 0, 0, 1, 0, 0, 0, 1, 0, 5, 5, 5, 5, 5, 5, 6, 6, 6};
    const IndexedMesh mesh = buildIndexedMesh(soup);
    CHECK(mesh.triangleCount() == 1);
}

TEST_CASE("every vertex belongs to one face and all faces are used") {
    const IndexedMesh mesh = buildIndexedMesh(cube());
    std::set<u32> used(mesh.faceOfVertex.begin(), mesh.faceOfVertex.end());
    CHECK(used.size() == mesh.faceCount);
    for (usize t = 0; t < mesh.triangleCount(); ++t) {
        const u32 face = mesh.faceOfVertex[mesh.indices[t * 3]];
        CHECK(mesh.faceOfVertex[mesh.indices[t * 3 + 1]] == face);
        CHECK(mesh.faceOfVertex[mesh.indices[t * 3 + 2]] == face);
    }
}

TEST_CASE("small regions merge into neighbours but creases stay sharp") {
    // Cube plus a fine tube: with a low face limit the small cube faces merge.
    TriangleSoup soup = cube();
    RegionOptions options;
    options.maxLargeFaces = 1; // every region below the whole mesh counts as small
    const IndexedMesh mesh = buildIndexedMesh(soup, options);
    CHECK(mesh.faceCount == 1);
    CHECK(mesh.vertexCount() == 24); // normals are still split at the creases
}

TEST_CASE("separate pieces never merge") {
    TriangleSoup soup = cube();
    TriangleSoup other = cube();
    for (usize i = 0; i < other.corners.size(); i += 3) {
        other.corners[i] += 5.0f;
    }
    soup.corners.insert(soup.corners.end(), other.corners.begin(), other.corners.end());
    RegionOptions options;
    options.maxLargeFaces = 1;
    CHECK(buildIndexedMesh(soup, options).faceCount == 2);
}

TEST_CASE("a connected noisy surface breaks into many faces, not one") {
    // A ridged strip where each ridge is a sharp crease, so every triangle starts in its own small
    // group. Merging must stop once regions are big enough rather than chaining into a single face.
    TriangleSoup soup;
    constexpr int kRidges = 1000;
    for (int i = 0; i < kRidges; ++i) {
        const float x = float(i), height = i % 2 == 0 ? 0.0f : 1.0f;
        const float nextHeight = i % 2 == 0 ? 1.0f : 0.0f;
        addQuad(soup.corners, {x, 0, height}, {x + 1, 0, nextHeight}, {x + 1, 1, nextHeight},
                {x, 1, height});
    }
    RegionOptions options;
    options.maxLargeFaces = 50; // regions under 40 triangles count as small
    const IndexedMesh mesh = buildIndexedMesh(soup, options);
    CHECK(mesh.faceCount > 10);
    CHECK(mesh.faceCount <= 1000);
}

TEST_CASE("a cube is closed and a single triangle is open") {
    CHECK(buildIndexedMesh(cube()).closed);
    TriangleSoup soup;
    soup.corners = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    CHECK_FALSE(buildIndexedMesh(soup).closed);
}
