#include "mesh/packer.h"

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <numbers>

using namespace geo;
using namespace geo::import;

namespace {

using Point = std::array<float, 3>;

// Adds a quad as its own face, with its own vertices and a flat normal.
void addQuad(IndexedMesh& mesh, u32 face, const std::array<Point, 4>& corners, Point normal) {
    const u32 base = u32(mesh.vertexCount());
    for (const Point& p : corners) {
        mesh.positions.insert(mesh.positions.end(), p.begin(), p.end());
        mesh.normals.insert(mesh.normals.end(), normal.begin(), normal.end());
        mesh.faceOfVertex.push_back(face);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    mesh.faceCount = std::max(mesh.faceCount, face + 1);
}

IndexedMesh unitCube() {
    IndexedMesh mesh;
    addQuad(mesh, 0, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}, {0, 0, -1});
    addQuad(mesh, 1, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}, {0, 0, 1});
    addQuad(mesh, 2, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}, {0, -1, 0});
    addQuad(mesh, 3, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}, {0, 1, 0});
    addQuad(mesh, 4, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}, {-1, 0, 0});
    addQuad(mesh, 5, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}, {1, 0, 0});
    mesh.closed = true;
    return mesh;
}

u32 edgeClass(const EdgeSegment& s) {
    return s.faceB >> kEdgeClassShift;
}
u32 otherFace(const EdgeSegment& s) {
    return s.faceB & kEdgeNoFace;
}

float length(const PackedDefinition& packed, const EdgeSegment& s) {
    const Point a = decodePosition(packed, s.a), b = decodePosition(packed, s.b);
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

} // namespace

TEST_CASE("a cube has twelve creases, each between two different faces") {
    const PackedDefinition packed = packMesh(unitCube());
    REQUIRE(packed.edgeSegments.size() == 12);
    float total = 0.0f;
    for (const EdgeSegment& s : packed.edgeSegments) {
        CHECK(edgeClass(s) == kEdgeCrease);
        CHECK(otherFace(s) != kEdgeNoFace);
        CHECK(otherFace(s) != s.faceA);
        total += length(packed, s);
    }
    CHECK(total == doctest::Approx(12.0f).epsilon(1e-4));
    REQUIRE(packed.edgeChunks.size() == 1);
    CHECK(packed.edgeChunks[0].segmentCount == 12);
}

TEST_CASE("an open square is bounded by one chain of boundary edges") {
    IndexedMesh mesh;
    addQuad(mesh, 0, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {0, 0, 1});
    const PackedDefinition packed = packMesh(mesh);
    REQUIRE(packed.edgeSegments.size() == 4);
    for (usize i = 0; i < 4; ++i) {
        const EdgeSegment& s = packed.edgeSegments[i];
        CHECK(edgeClass(s) == kEdgeBoundary);
        CHECK(otherFace(s) == kEdgeNoFace);
        // One loop, so arc length runs on from segment to segment.
        CHECK(s.along == doctest::Approx(float(i)).epsilon(1e-4));
        if (i > 0) {
            const Point end = decodePosition(packed, packed.edgeSegments[i - 1].b);
            const Point begin = decodePosition(packed, s.a);
            CHECK(end == begin);
        }
    }
}

TEST_CASE("faces meeting tangentially make a smooth edge, and a seam inside one face is dropped") {
    IndexedMesh twoFaces;
    addQuad(twoFaces, 0, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {0, 0, 1});
    addQuad(twoFaces, 1, {{{1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}}}, {0, 0, 1});
    const PackedDefinition packed = packMesh(twoFaces);
    usize smooth = 0, boundary = 0;
    for (const EdgeSegment& s : packed.edgeSegments) {
        smooth += edgeClass(s) == kEdgeSmooth ? 1 : 0;
        boundary += edgeClass(s) == kEdgeBoundary ? 1 : 0;
    }
    CHECK(smooth == 1);
    CHECK(boundary == 6);

    IndexedMesh oneFace = twoFaces;
    std::fill(oneFace.faceOfVertex.begin(), oneFace.faceOfVertex.end(), 0u);
    oneFace.faceCount = 1;
    const PackedDefinition seamed = packMesh(oneFace);
    CHECK(seamed.edgeSegments.size() == 6);
    for (const EdgeSegment& s : seamed.edgeSegments) {
        CHECK(edgeClass(s) == kEdgeBoundary);
    }
}

TEST_CASE("a faceted cylinder from a mesh file has two crease loops around its caps") {
    constexpr int kSides = 48;
    TriangleSoup soup;
    const auto add = [&](Point a, Point b, Point c) {
        for (const Point& p : {a, b, c}) {
            soup.corners.insert(soup.corners.end(), p.begin(), p.end());
        }
    };
    const auto rim = [](int i, float z) {
        const float angle = 2.0f * std::numbers::pi_v<float> * float(i % kSides) / kSides;
        return Point{std::cos(angle), std::sin(angle), z};
    };
    for (int i = 0; i < kSides; ++i) {
        add(rim(i, 0), rim(i + 1, 0), rim(i + 1, 2));
        add(rim(i, 0), rim(i + 1, 2), rim(i, 2));
        add({0, 0, 2}, rim(i, 2), rim(i + 1, 2));
        add({0, 0, 0}, rim(i + 1, 0), rim(i, 0));
    }
    const PackedDefinition packed = packMesh(buildIndexedMesh(soup));
    REQUIRE(packed.edgeSegments.size() == 2 * kSides);
    float longestRun = 0.0f;
    for (const EdgeSegment& s : packed.edgeSegments) {
        CHECK(edgeClass(s) == kEdgeCrease);
        longestRun = std::max(longestRun, s.along + length(packed, s));
    }
    // Each loop is one chain, so its arc length reaches the polygon's perimeter.
    const float perimeter = 2.0f * kSides * std::sin(std::numbers::pi_v<float> / kSides);
    CHECK(longestRun == doctest::Approx(perimeter).epsilon(1e-3));
    CHECK(packed.edgeChunks.size() == 3);
}
