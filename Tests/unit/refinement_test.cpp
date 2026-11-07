#include "render/refinement.h"

#include <doctest/doctest.h>

using namespace geo;
using namespace geo::render;

namespace {

nav::Camera orthographic(double halfHeight) {
    nav::Camera camera;
    camera.lookAt(simd_make_double3(0, 0, 10), simd_make_double3(0, 0, 0),
                  simd_make_double3(0, 1, 0));
    camera.orthoHalfHeight = halfHeight;
    return camera;
}

const nav::Viewport kViewport{1000.0, 1000.0};

} // namespace

TEST_CASE("a fitted view needs no refinement") {
    // 1000 pixels over 2 units is 500 pixels per unit, so 0.001 units of error is half a pixel.
    const PlacedExtent placed[] = {{simd_make_double3(0, 0, 0), 1.0, 0}};
    const std::vector<u32> levels =
        neededLevels(placed, 1, orthographic(1.0), kViewport, {0.001, 4, 0.5});
    CHECK(levels[0] == 0);
}

TEST_CASE("each doubling of zoom asks for one more level") {
    const PlacedExtent placed[] = {{simd_make_double3(0, 0, 0), 1.0, 0}};
    CHECK(neededLevels(placed, 1, orthographic(0.5), kViewport, {0.001, 4, 0.5})[0] == 1);
    CHECK(neededLevels(placed, 1, orthographic(0.25), kViewport, {0.001, 4, 0.5})[0] == 2);
    CHECK(neededLevels(placed, 1, orthographic(0.001), kViewport, {0.001, 4, 0.5})[0] == 4);
}

TEST_CASE("only visible definitions are refined") {
    const PlacedExtent placed[] = {{simd_make_double3(0, 0, 0), 0.1, 0},
                                   {simd_make_double3(5, 0, 0), 0.1, 1}};
    const std::vector<u32> levels =
        neededLevels(placed, 2, orthographic(0.25), kViewport, {0.001, 4, 0.5});
    CHECK(levels[0] == 2);
    CHECK(levels[1] == 0);
}

TEST_CASE("the nearest occurrence decides in perspective") {
    nav::Camera camera = orthographic(1.0);
    camera.perspective = true;
    const PlacedExtent placed[] = {{simd_make_double3(0, 0, 9), 0.1, 0},
                                   {simd_make_double3(0, 0, -100), 0.1, 0}};
    const std::vector<u32> near = neededLevels(placed, 1, camera, kViewport, {0.001, 8, 0.5});
    const std::vector<u32> far =
        neededLevels(std::span(placed).subspan(1), 1, camera, kViewport, {0.001, 8, 0.5});
    CHECK(near[0] > far[0]);
}

TEST_CASE("a model without tolerances is never refined") {
    const PlacedExtent placed[] = {{simd_make_double3(0, 0, 0), 1.0, 0}};
    CHECK(neededLevels(placed, 1, orthographic(0.001), kViewport, {0.0, 4, 0.5})[0] == 0);
}
