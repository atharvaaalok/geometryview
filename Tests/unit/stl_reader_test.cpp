#include "stl/stl_reader.h"

#include "import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace geo;
using namespace geo::import;

namespace {

std::span<const u8> bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

std::vector<u8> binaryStl(const std::vector<float>& corners, std::string_view header = "binary") {
    const u32 triangles = u32(corners.size() / 9);
    std::vector<u8> file(84 + usize(triangles) * 50, 0);
    std::memcpy(file.data(), header.data(), header.size());
    std::memcpy(file.data() + 80, &triangles, sizeof(triangles));
    for (u32 t = 0; t < triangles; ++t) {
        std::memcpy(file.data() + 84 + usize(t) * 50 + 12, corners.data() + usize(t) * 9, 36);
    }
    return file;
}

constexpr std::string_view kAsciiTriangle = R"(solid part
  facet normal 0 0 1
    outer loop
      vertex 0 0 0
      vertex 1.5 0 -2.5e1
      vertex 0 1E2 .25
    endloop
  endfacet
endsolid part
)";

} // namespace

TEST_CASE("binary STL reads every corner") {
    const std::vector<float> corners = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 2, 1, 1, 1, 2, 1};
    const std::vector<u8> file = binaryStl(corners);
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(file, soup, error));
    CHECK(soup.corners == corners);
    CHECK(soup.warnings.empty());
}

TEST_CASE("a binary file whose header starts with solid is still binary") {
    const std::vector<u8> file = binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0}, "solid exported by CAD");
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(file, soup, error));
    CHECK(soup.triangleCount() == 1);
}

TEST_CASE("ASCII STL parses integers, decimals and exponents") {
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(bytes(kAsciiTriangle), soup, error));
    const std::vector<float> expected = {0, 0, 0, 1.5f, 0, -25, 0, 100, 0.25f};
    CHECK(soup.corners == expected);
}

TEST_CASE("ASCII STL tolerates a color line, carriage returns and a missing endsolid") {
    constexpr std::string_view text = "solid part\r\n"
                                      "color 0.1 0.2 0.3\r\n"
                                      "facet normal 0 0 1\r\n"
                                      "outer loop\r\n"
                                      "\tvertex 0 0 0\r\n"
                                      "\tvertex 1 0 0\r\n"
                                      "\tvertex 0 1 0\r\n"
                                      "endloop\r\n"
                                      "endfacet\r\n";
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(bytes(text), soup, error));
    CHECK(soup.triangleCount() == 1);
}

TEST_CASE("ASCII parsing rounds like the standard library") {
    constexpr std::string_view text = "solid s\n"
                                      "vertex 0.1 3.14159265358979 123456789012345678901234567890\n"
                                      "vertex 1e-30 -0.000001 1.17549435e-38\n"
                                      "vertex 9007199254740993 2.5 0.30000000000000004\n";
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(bytes(text), soup, error));
    const std::vector<float> expected = {0.1f,
                                         3.14159265358979f,
                                         123456789012345678901234567890.0f,
                                         1e-30f,
                                         -0.000001f,
                                         1.17549435e-38f,
                                         9007199254740993.0f,
                                         2.5f,
                                         0.30000000000000004f};
    REQUIRE(soup.corners.size() == expected.size());
    for (usize i = 0; i < expected.size(); ++i) {
        CHECK(soup.corners[i] == doctest::Approx(expected[i]).epsilon(1e-6));
    }
}

TEST_CASE("large ASCII files parse the same in parallel as line by line") {
    std::string text = "solid big\n";
    std::vector<float> expected;
    constexpr int kTriangles = 300'000;
    text.reserve(usize(kTriangles) * 100);
    for (int t = 0; t < kTriangles; ++t) {
        for (int corner = 0; corner < 3; ++corner) {
            const float x = float(t), y = float(corner), z = float(t % 7);
            text += "vertex " + std::to_string(int(x)) + " " + std::to_string(int(y)) + " " +
                    std::to_string(int(z)) + "\n";
            expected.insert(expected.end(), {x, y, z});
        }
    }
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(bytes(text), soup, error));
    CHECK(soup.corners == expected);
}

TEST_CASE("a truncated ASCII file keeps complete triangles and warns") {
    constexpr std::string_view text = "solid s\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                                      "vertex 5 5 5\n";
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(bytes(text), soup, error));
    CHECK(soup.triangleCount() == 1);
    CHECK(soup.warnings.size() == 1);
}

TEST_CASE("triangles with NaN are dropped with a warning") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<u8> file =
        binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0, nan, 0, 0, 1, 0, 0, 0, 1, 0});
    TriangleSoup soup;
    std::string error;
    REQUIRE(readStl(file, soup, error));
    CHECK(soup.triangleCount() == 1);
    CHECK(soup.warnings.size() == 1);
}

TEST_CASE("files that are not STL fail with a readable message") {
    TriangleSoup soup;
    std::string error;
    CHECK_FALSE(readStl(bytes(""), soup, error));
    CHECK(error == "This file is empty.");
    CHECK_FALSE(readStl(bytes("hello"), soup, error));
    CHECK(error == "This does not look like an STL file.");
    CHECK_FALSE(readStl(bytes("solid only\nendsolid only\n"), soup, error));
}

TEST_CASE("a binary count larger than the file is rejected") {
    std::vector<u8> file = binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0});
    const u32 absurd = 0xFFFFFFFF;
    std::memcpy(file.data() + 80, &absurd, sizeof(absurd));
    TriangleSoup soup;
    std::string error;
    CHECK_FALSE(readStl(file, soup, error));
}

namespace {

// Reads `file` in pieces of the sizes given, cycling through them.
bool readInPieces(std::span<const u8> file, std::span<const usize> sizes, TriangleSoup& soup,
                  std::string& error) {
    StlPieces pieces;
    if (!pieces.begin(file.first(std::min<usize>(file.size(), 4096)), file.size())) {
        error = "not in pieces";
        return false;
    }
    for (usize at = 0, i = 0; at < file.size(); ++i) {
        const usize size = std::min(sizes[i % sizes.size()], file.size() - at);
        pieces.append(file.subspan(at, size));
        at += size;
    }
    return pieces.finish(soup, error);
}

std::vector<float> manyCorners(usize triangles) {
    std::vector<float> corners(triangles * 9);
    for (usize i = 0; i < corners.size(); ++i) {
        corners[i] = float(i % 977) * 0.25f - float(i % 13);
    }
    return corners;
}

} // namespace

TEST_CASE("a binary STL read in pieces makes the soup readStl makes") {
    const std::vector<u8> file = binaryStl(manyCorners(70'000));
    TriangleSoup whole;
    std::string error;
    REQUIRE(readStl(file, whole, error));
    // Pieces that split the header, split records anywhere, and hold many records at once.
    const std::vector<std::vector<usize>> patterns = {
        {1}, {7, 49, 50, 51}, {83, 1, 50}, {84}, {85, 4096}, {1 << 20}, {file.size()}};
    for (const std::vector<usize>& sizes : patterns) {
        CAPTURE(sizes[0]);
        TriangleSoup soup;
        REQUIRE_MESSAGE(readInPieces(file, sizes, soup, error), error);
        CHECK(soup.corners == whole.corners);
        CHECK(soup.warnings == whole.warnings);
    }
}

TEST_CASE("an STL read in pieces drops invalid triangles as readStl does") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<u8> file =
        binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0, nan, 0, 0, 1, 0, 0, 0, 1, 0});
    TriangleSoup soup;
    std::string error;
    const std::vector<usize> sizes = {13};
    REQUIRE(readInPieces(file, sizes, soup, error));
    CHECK(soup.triangleCount() == 1);
    CHECK(soup.warnings.size() == 1);
}

TEST_CASE("only an exact binary STL is read in pieces") {
    StlPieces pieces;
    CHECK_FALSE(pieces.begin(bytes(kAsciiTriangle), kAsciiTriangle.size()));
    std::vector<u8> longer = binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0});
    longer.push_back(0);
    CHECK_FALSE(pieces.begin(longer, longer.size()));
    const std::vector<u8> exact = binaryStl({0, 0, 0, 1, 0, 0, 0, 1, 0});
    CHECK_FALSE(pieces.begin(std::span(exact).first(40), exact.size()));
    CHECK(pieces.begin(exact, exact.size()));
}

TEST_CASE("an STL read in pieces that ends early fails") {
    const std::vector<u8> file = binaryStl(manyCorners(10));
    StlPieces pieces;
    REQUIRE(pieces.begin(file, file.size()));
    pieces.append(std::span(file).first(file.size() - 20));
    TriangleSoup soup;
    std::string error;
    CHECK_FALSE(pieces.finish(soup, error));
    CHECK(error == "The STL file ended early.");
}

TEST_CASE("a file hashed in pieces hashes as it does whole") {
    const std::vector<u8> file = binaryStl(manyCorners(5'000));
    ContentHash hash;
    for (usize at = 0; at < file.size(); at += 777) {
        hash.update(std::span(file).subspan(at, std::min<usize>(777, file.size() - at)));
    }
    CHECK(hash.digest() == hashContents(file));
}
