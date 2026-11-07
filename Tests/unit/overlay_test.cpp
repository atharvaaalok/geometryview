#include "render/overlay.h"

#include <cstring>
#include <doctest/doctest.h>

using namespace geo;
using namespace geo::render;

TEST_CASE("the scale bar picks round lengths in sensible units") {
    // At 10 pixels per millimetre, 96 pixels allows 9.6 mm, so the bar is 5 mm.
    ScaleChoice choice = chooseScale(10.0, 96.0);
    CHECK(choice.length == doctest::Approx(5.0));
    CHECK(std::strcmp(choice.label, "5 mm") == 0);

    choice = chooseScale(0.05, 96.0); // 1920 mm allowed
    CHECK(choice.length == doctest::Approx(1000.0));
    CHECK(std::strcmp(choice.label, "1 m") == 0);

    choice = chooseScale(5000.0, 96.0); // 0.0192 mm allowed
    CHECK(choice.length == doctest::Approx(0.01));
    CHECK(std::strcmp(choice.label, "10 um") == 0);

    choice = chooseScale(4.0, 96.0); // 24 mm allowed
    CHECK(choice.length == doctest::Approx(20.0));
}

TEST_CASE("the overlay sits in the top and bottom right corners") {
    nav::Camera camera;
    camera.orthoHalfHeight = 50.0;
    const nav::Viewport viewport{2000.0, 1200.0};
    const Overlay overlay = makeOverlay(camera, viewport, matrix_identity_float3x3, 2.0f, 13.0f, 0);
    CHECK(overlay.visible);
    CHECK(overlay.uniforms.cubeCenter.x == doctest::Approx(2000 - (6 + 66) * 2));
    CHECK(overlay.uniforms.cubeCenter.y == doctest::Approx((12 + 66) * 2));
    REQUIRE(overlay.uniforms.quadCount >= 3);
    // The bar's right end is 24 points from the edge.
    const OverlayQuad& bar = overlay.quads[0];
    CHECK(bar.origin.x + bar.size.x == doctest::Approx(2000 - 24 * 2));
}
