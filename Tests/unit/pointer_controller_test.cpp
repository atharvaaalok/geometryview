#include "nav/pointer_controller.h"

#include <doctest/doctest.h>
#include <optional>

using namespace geo;
using namespace geo::nav;

namespace {

struct Rig {
    Camera camera;
    Viewport viewport{1000, 1000};
    OrbitPolicy orbit;
    std::optional<SurfaceHit> hit;
    int picks = 0;
    PointerController controller;

    Rig()
        : controller(camera, viewport, orbit, [this](double, double) {
              ++picks;
              return hit;
          }) {
        camera.orthoHalfHeight = 2.0;
        camera.lookAt(simd_make_double3(0, 0, 5), simd_make_double3(0, 0, 0),
                      simd_make_double3(0, 1, 0));
        orbit.setVisibleBounds({simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)});
    }
};

} // namespace

TEST_CASE("a press and release without moving is a click") {
    Rig rig;
    rig.hit = SurfaceHit{7, simd_make_double3(0, 0, 1), 4.0};
    rig.controller.press(Button::Left, 0, 500, 500);
    const PointerResponse response = rig.controller.release(Button::Left, 500, 500);
    CHECK(response.clicked);
    CHECK(response.clickedId == 7);
}

TEST_CASE("clicking empty space reports a click with no id") {
    Rig rig;
    rig.controller.press(Button::Left, 0, 500, 500);
    const PointerResponse response = rig.controller.release(Button::Left, 500, 500);
    CHECK(response.clicked);
    CHECK_FALSE(response.clickedId.has_value());
}

TEST_CASE("small movement under the threshold stays a click") {
    Rig rig;
    const Vec3 before = rig.camera.position;
    rig.controller.press(Button::Left, 0, 500, 500);
    CHECK_FALSE(rig.controller.move(503, 500).cameraMoved);
    CHECK(simd_distance(rig.camera.position, before) == 0.0);
    CHECK(rig.controller.release(Button::Left, 503, 500).clicked);
}

TEST_CASE("the threshold is in points, so it scales with the display") {
    Rig rig;
    rig.controller.setPixelsPerPoint(2.0);
    rig.controller.press(Button::Left, 0, 500, 500);
    CHECK_FALSE(rig.controller.move(508, 500).cameraMoved); // 4 points
    CHECK(rig.controller.move(511, 500).cameraMoved);       // 5.5 points
}

TEST_CASE("left drag rotates and does not click") {
    Rig rig;
    rig.controller.press(Button::Left, 0, 500, 500);
    CHECK(rig.controller.move(600, 500).cameraMoved);
    CHECK(rig.controller.isDragging());
    CHECK(rig.camera.position.x < 0.0);
    CHECK_FALSE(rig.controller.release(Button::Left, 600, 500).clicked);
    CHECK_FALSE(rig.controller.isDragging());
}

TEST_CASE("a cancelled press ends without a click, and later moves turn nothing") {
    Rig rig;
    rig.controller.press(Button::Left, 0, 500, 500);
    CHECK(rig.controller.move(600, 500).cameraMoved);
    rig.controller.cancel();
    CHECK_FALSE(rig.controller.isPressed());
    CHECK_FALSE(rig.controller.isDragging());
    const Vec3 before = rig.camera.position;
    CHECK_FALSE(rig.controller.move(700, 500).cameraMoved);
    CHECK(simd_distance(rig.camera.position, before) == 0.0);
    CHECK_FALSE(rig.controller.release(Button::Left, 700, 500).clicked);
}

TEST_CASE("a drag resolves its pivot once, however far it runs") {
    Rig rig;
    rig.orbit.setMode(PivotMode::Pointer);
    rig.hit = SurfaceHit{1, simd_make_double3(0.5, 0, 1), 4.0};
    rig.controller.press(Button::Left, 0, 500, 500);
    rig.controller.move(520, 500);
    const int picksAtStart = rig.picks;
    rig.controller.move(540, 500);
    rig.controller.move(560, 500);
    CHECK(rig.picks == picksAtStart);
}

TEST_CASE("middle drag pans one to one") {
    Rig rig;
    rig.hit = SurfaceHit{1, simd_make_double3(0, 0, 0), 5.0};
    const Vec3 point = simd_make_double3(0, 0, 0);
    rig.controller.press(Button::Middle, 0, 500, 500);
    rig.controller.move(510, 500); // crosses the threshold and starts the gesture
    const NdcPoint before = rig.camera.project(point, rig.viewport);
    rig.controller.move(610, 450);
    const NdcPoint after = rig.camera.project(point, rig.viewport);
    CHECK((after.x - before.x) * 500.0 == doctest::Approx(100.0));
    CHECK((after.y - before.y) * 500.0 == doctest::Approx(50.0));
}

TEST_CASE("right drag dollies about the target") {
    Rig rig;
    rig.controller.press(Button::Right, 0, 500, 500);
    rig.controller.move(500, 450);
    rig.controller.move(500, 400);
    CHECK(rig.camera.orthoHalfHeight < 2.0);
}

TEST_CASE("a MacBook can pan with shift or space held") {
    SUBCASE("shift") {
        Rig rig;
        const Vec3 before = rig.camera.position;
        rig.controller.press(Button::Left, kModifierShift, 500, 500);
        rig.controller.move(510, 500);
        rig.controller.move(560, 500);
        CHECK(rig.camera.orthoHalfHeight == doctest::Approx(2.0));
        CHECK(rig.camera.position.z == doctest::Approx(before.z));
        CHECK(rig.camera.position.x < before.x);
    }
    SUBCASE("space") {
        Rig rig;
        rig.controller.press(Button::Left, kModifierSpace, 500, 500);
        rig.controller.move(510, 500);
        rig.controller.move(560, 500);
        CHECK(rig.camera.position.z == doctest::Approx(5.0));
    }
}

TEST_CASE("the most specific binding wins") {
    Rig rig;
    std::vector<MouseBinding> bindings = {{Button::Left, 0, Gesture::Rotate},
                                          {Button::Left, kModifierOption, Gesture::Dolly}};
    rig.controller.setBindings(std::move(bindings));
    rig.controller.press(Button::Left, kModifierOption, 500, 500);
    rig.controller.move(500, 450);
    rig.controller.move(500, 400);
    CHECK(rig.camera.orthoHalfHeight < 2.0);
}

TEST_CASE("an unbound button does nothing") {
    Rig rig;
    rig.controller.setBindings({});
    const Vec3 before = rig.camera.position;
    rig.controller.press(Button::Middle, 0, 500, 500);
    CHECK_FALSE(rig.controller.move(600, 600).cameraMoved);
    CHECK(simd_distance(rig.camera.position, before) == 0.0);
}

TEST_CASE("scrolling up zooms in about the surface under the pointer") {
    Rig rig;
    const Vec3 point = simd_make_double3(0.5, 0.5, 0);
    rig.hit = SurfaceHit{1, point, 5.0};
    const NdcPoint before = rig.camera.project(point, rig.viewport);
    const double x = (before.x + 1.0) * 500.0, y = (1.0 - before.y) * 500.0;
    CHECK(rig.controller.scroll(100, x, y, 1.0).cameraMoved);
    CHECK(rig.camera.orthoHalfHeight < 2.0);
    const NdcPoint after = rig.camera.project(point, rig.viewport);
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
}

TEST_CASE("a wheel burst locks its anchor, because the depth buffer lags") {
    Rig rig;
    rig.hit = SurfaceHit{1, simd_make_double3(0, 0, 0), 5.0};
    rig.controller.scroll(100, 500, 500, 1.0);
    const int picks = rig.picks;
    rig.controller.scroll(100, 500, 500, 1.05);
    rig.controller.scroll(100, 500, 500, 1.10);
    CHECK(rig.picks == picks);

    rig.controller.scroll(100, 500, 500, 2.0); // a pause starts a new burst
    CHECK(rig.picks == picks + 1);
    rig.controller.scroll(100, 510, 500, 2.05); // moving the pointer does too
    CHECK(rig.picks == picks + 2);
}

TEST_CASE("zooming over empty space uses the focus depth") {
    Rig rig;
    CHECK(rig.controller.scroll(100, 800, 800, 1.0).cameraMoved);
    CHECK(rig.camera.orthoHalfHeight < 2.0);
}

TEST_CASE("pan and zoom re-evaluate the pivot, rotation does not") {
    Rig rig;
    rig.camera.orthoHalfHeight = 0.5; // close up
    rig.hit = SurfaceHit{1, simd_make_double3(0, 0, 1), 4.0};
    rig.controller.press(Button::Left, 0, 500, 500);
    rig.controller.move(520, 500);
    rig.controller.release(Button::Left, 520, 500);
    CHECK(rig.orbit.closeUp());
    const Vec3 first = rig.orbit.pivot();

    rig.hit = SurfaceHit{1, simd_make_double3(0.3, 0, 1), 4.0};
    rig.controller.press(Button::Left, 0, 500, 500);
    rig.controller.move(520, 500);
    rig.controller.release(Button::Left, 520, 500);
    CHECK(simd_distance(rig.orbit.pivot(), first) == 0.0); // successive rotations keep it

    rig.controller.scroll(10, 500, 500, 5.0);
    rig.controller.press(Button::Left, 0, 500, 500);
    rig.controller.move(520, 500);
    rig.controller.release(Button::Left, 520, 500);
    CHECK(rig.orbit.pivot().x == doctest::Approx(0.3));
}
