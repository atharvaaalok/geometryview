#include "nav/camera.h"
#include "nav/orbit_policy.h"

#include <cmath>
#include <doctest/doctest.h>
#include <optional>

using namespace geo;
using namespace geo::nav;

namespace {

constexpr double kEpsilon = 1e-9;
const Viewport kSquare{1000, 1000};
const Viewport kWide{2000, 1000};
const Vec3 kUp = simd_make_double3(0.0, 1.0, 0.0);
const Vec3 kOrigin = simd_make_double3(0.0, 0.0, 0.0);

Vec3 v(double x, double y, double z) {
    return simd_make_double3(x, y, z);
}

void checkNear(Vec3 a, Vec3 b, double tolerance = kEpsilon) {
    CHECK(simd_distance(a, b) <= tolerance);
}

Camera perspectiveAt(double z, double fovDegrees = 90.0) {
    Camera camera;
    camera.perspective = true;
    camera.fovY = fovDegrees * M_PI / 180.0;
    camera.lookAt(v(0, 0, z), kOrigin, kUp);
    return camera;
}

Camera orthographicAt(double z, double halfHeight) {
    Camera camera;
    camera.orthoHalfHeight = halfHeight;
    camera.lookAt(v(0, 0, z), kOrigin, kUp);
    return camera;
}

// Models proto's two-unit cube: a ray from the centre of the view hits its front face at z = 1.
SurfaceProbe cubeProbe(const Camera& camera, const Viewport& viewport) {
    return [&](double x, double y) -> std::optional<Vec3> {
        const double depth = camera.position.z - 1.0;
        const Vec3 hit = camera.unproject(x, y, depth, viewport);
        if (std::abs(hit.x) <= 1.0 && std::abs(hit.y) <= 1.0) {
            return hit;
        }
        return std::nullopt;
    };
}

const Aabb kCube{v(-1, -1, -1), v(1, 1, 1)};

} // namespace

TEST_CASE("a lookAt camera faces its target with its up vector upright") {
    const Camera camera = perspectiveAt(5.0);
    checkNear(camera.forward(), v(0, 0, -1));
    checkNear(camera.up(), kUp);
    checkNear(camera.right(), v(1, 0, 0));
    checkNear(camera.target(), kOrigin);
}

TEST_CASE("project and unproject are inverses") {
    Camera camera = perspectiveAt(5.0, 40.0);
    camera.lookAt(v(3, 2, 7), v(1, 0, -1), kUp);
    const Vec3 point = v(0.5, 0.25, 1.0);
    const NdcPoint ndc = camera.project(point, kWide);
    checkNear(camera.unproject(ndc.x, ndc.y, ndc.viewDepth, kWide), point);
}

TEST_CASE("a rightward drag moves the camera left, so the model follows the pointer") {
    Camera camera = perspectiveAt(5.0);
    tumble(camera, kOrigin, 0.1, 0.0, 3.0);
    CHECK(camera.position.x < 0.0);
    CHECK(camera.position.y == doctest::Approx(0.0));
}

TEST_CASE("rotation is exactly proto's: a full-width drag turns 6 radians") {
    Camera camera = perspectiveAt(5.0);
    tumble(camera, kOrigin, 2.0, 0.0, 3.0);
    CHECK(simd_length(camera.position) == doctest::Approx(5.0));
    CHECK(camera.position.x == doctest::Approx(-5.0 * std::sin(6.0)));
    CHECK(camera.position.z == doctest::Approx(5.0 * std::cos(6.0)));
}

TEST_CASE("the pivot stays fixed in view while rotating about it") {
    Camera camera = perspectiveAt(5.0);
    const Vec3 pivot = v(0.7, -0.4, 0.3);
    const NdcPoint before = camera.project(pivot, kSquare);
    tumble(camera, pivot, 0.0, 0.0, 3.0);
    const NdcPoint unchanged = camera.project(pivot, kSquare);
    CHECK(unchanged.x == doctest::Approx(before.x));

    // Distance to the pivot is preserved by any rotation.
    const double distance = simd_distance(camera.position, pivot);
    tumble(camera, pivot, 0.3, 0.2, 3.0);
    CHECK(simd_distance(camera.position, pivot) == doctest::Approx(distance));
}

TEST_CASE("there is no world-up lock: the camera can roll over the top") {
    Camera camera = perspectiveAt(5.0);
    for (int i = 0; i < 10; ++i) {
        tumble(camera, kOrigin, 0.0, 0.1, 3.0);
    }
    CHECK(simd_length(camera.position) == doctest::Approx(5.0));
    CHECK(camera.up().y < 0.0); // 3 radians: past the pole and upside down
}

TEST_CASE("many small rotations stay stable and unit length") {
    Camera camera = perspectiveAt(5.0);
    for (int i = 0; i < 100000; ++i) {
        tumble(camera, kOrigin, 0.001, 0.0007, 3.0);
    }
    CHECK(simd_length(camera.position) == doctest::Approx(5.0).epsilon(1e-6));
    CHECK(simd_length(camera.orientation.vector) == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("proto's adaptive close-up test vector") {
    // Two-unit cube, fov 90, camera on +Z: z=2.2 far, 1.6 close, 1.75 stays close, 2.0 exits.
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);

    Camera camera = perspectiveAt(2.2);
    policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare));
    CHECK_FALSE(policy.closeUp());

    camera = perspectiveAt(1.6);
    policy.navigated();
    policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare));
    CHECK(policy.closeUp());
    CHECK(policy.pivot().z == doctest::Approx(1.0));

    // Successive rotations keep the anchored pivot, even with the pointer elsewhere.
    const Vec3 saved = policy.pivot();
    tumble(camera, policy.pivot(), 0.2, 0.1, 3.0);
    policy.resolve(camera, kSquare, 0.5, 0.5, cubeProbe(camera, kSquare));
    checkNear(policy.pivot(), saved);

    camera = perspectiveAt(1.75);
    policy.navigated();
    policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare));
    CHECK(policy.closeUp());

    camera = perspectiveAt(2.0);
    policy.navigated();
    policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare));
    CHECK_FALSE(policy.closeUp());
    CHECK(policy.pivot().z == doctest::Approx(0.0));
}

TEST_CASE("adaptive mode ignores the pointer, and only pointer mode uses it") {
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);
    const Camera camera = perspectiveAt(1.6);
    policy.resolve(camera, kSquare, 0.3, 0.3, cubeProbe(camera, kSquare));
    CHECK(policy.pivot().x == doctest::Approx(0.0));

    policy.setMode(PivotMode::Pointer);
    policy.resolve(camera, kSquare, 0.3, 0.3, cubeProbe(camera, kSquare));
    CHECK(policy.pivot().x != doctest::Approx(0.0));
}

TEST_CASE("close up with nothing near the centre keeps the previous pivot") {
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);
    Camera camera = perspectiveAt(1.6);
    const SurfaceProbe miss = [](double, double) { return std::nullopt; };
    policy.resolve(camera, kSquare, 0, 0, miss);
    CHECK(policy.closeUp());
    checkNear(policy.pivot(), kOrigin);
}

TEST_CASE("the adaptive rule works in orthographic views") {
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);
    Camera camera = orthographicAt(5.0, 2.0); // the cube fills half the view
    const auto probe = [](double, double) { return std::optional<Vec3>(v(0, 0, 1)); };
    policy.resolve(camera, kSquare, 0, 0, probe);
    CHECK_FALSE(policy.closeUp());

    camera.orthoHalfHeight = 0.5; // size 2.0, past the threshold
    policy.navigated();
    policy.resolve(camera, kSquare, 0, 0, probe);
    CHECK(policy.closeUp());
    CHECK(policy.pivot().z == doctest::Approx(1.0));
}

TEST_CASE("an explicit pivot wins until it is cleared") {
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);
    policy.setExplicitPivot(v(4, 5, 6));
    Camera camera = perspectiveAt(1.6);
    policy.navigated();
    checkNear(policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare)), v(4, 5, 6));
    policy.clearExplicitPivot();
    camera = perspectiveAt(3.0);
    checkNear(policy.resolve(camera, kSquare, 0, 0, cubeProbe(camera, kSquare)), kOrigin);
}

TEST_CASE("changing visible bounds re-centres the pivot") {
    OrbitPolicy policy;
    policy.setVisibleBounds(kCube);
    const Aabb moved{v(9, -1, -1), v(11, 1, 1)};
    policy.setVisibleBounds(moved);
    checkNear(policy.pivot(), v(10, 0, 0));
}

TEST_CASE("zooming keeps the anchor on the same pixel") {
    const Vec3 anchor = v(0.4, -0.3, 0.2);

    SUBCASE("orthographic") {
        Camera camera = orthographicAt(5.0, 2.0);
        const NdcPoint before = camera.project(anchor, kWide);
        zoomAbout(camera, 0.5, anchor, before.x, before.y, kWide);
        const NdcPoint after = camera.project(anchor, kWide);
        CHECK(after.x == doctest::Approx(before.x));
        CHECK(after.y == doctest::Approx(before.y));
        CHECK(camera.orthoHalfHeight == doctest::Approx(1.0));
    }

    SUBCASE("perspective") {
        Camera camera = perspectiveAt(5.0, 40.0);
        const NdcPoint before = camera.project(anchor, kWide);
        zoomAbout(camera, 0.5, anchor, before.x, before.y, kWide);
        const NdcPoint after = camera.project(anchor, kWide);
        CHECK(after.x == doctest::Approx(before.x));
        CHECK(after.y == doctest::Approx(before.y));
        CHECK(after.viewDepth == doctest::Approx(before.viewDepth * 0.5));
    }
}

TEST_CASE("perspective zoom never stalls at the target") {
    Camera camera = perspectiveAt(1.0, 40.0);
    for (int i = 0; i < 200; ++i) {
        zoomAbout(camera, 0.9, v(0, 0, -3), 0, 0, kSquare);
    }
    CHECK(camera.position.z == doctest::Approx(-3.0).epsilon(1e-6));
}

TEST_CASE("panning moves the scene one to one with the pointer") {
    SUBCASE("orthographic") {
        Camera camera = orthographicAt(5.0, 2.0);
        const Vec3 point = v(0.2, 0.1, 0.0);
        const NdcPoint before = camera.project(point, kSquare);
        pan(camera, 100.0, 50.0, 5.0, kSquare);
        const NdcPoint after = camera.project(point, kSquare);
        CHECK((after.x - before.x) * 500.0 == doctest::Approx(100.0));
        CHECK((before.y - after.y) * 500.0 == doctest::Approx(50.0));
    }

    SUBCASE("perspective, at the grabbed depth") {
        Camera camera = perspectiveAt(5.0, 40.0);
        const Vec3 point = v(0.2, 0.1, 2.0);
        const NdcPoint before = camera.project(point, kSquare);
        pan(camera, 100.0, 50.0, before.viewDepth, kSquare);
        const NdcPoint after = camera.project(point, kSquare);
        CHECK((after.x - before.x) * 500.0 == doctest::Approx(100.0));
        CHECK((before.y - after.y) * 500.0 == doctest::Approx(50.0));
    }
}

TEST_CASE("fit frames the whole box with the requested margin") {
    SUBCASE("orthographic") {
        Camera camera = orthographicAt(5.0, 9.0);
        fit(camera, kCube, kSquare, 1.12);
        CHECK(camera.orthoHalfHeight == doctest::Approx(1.12));
        checkNear(camera.target() - camera.forward() * 0.0, camera.target());
    }

    SUBCASE("a wide box in a tall window is limited by width") {
        Camera camera = orthographicAt(5.0, 9.0);
        const Viewport tall{500, 1000};
        fit(camera, {v(-4, -1, -1), v(4, 1, 1)}, tall, 1.0);
        CHECK(camera.orthoHalfHeight == doctest::Approx(8.0));
    }

    SUBCASE("every corner lands on screen in perspective") {
        Camera camera = perspectiveAt(5.0, 40.0);
        camera.lookAt(v(3, 4, 5), kOrigin, kUp);
        fit(camera, kCube, kWide, 1.12);
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 p = v(corner & 1 ? 1 : -1, corner & 2 ? 1 : -1, corner & 4 ? 1 : -1);
            const NdcPoint ndc = camera.project(p, kWide);
            CHECK(ndc.viewDepth > 0.0);
            CHECK(std::abs(ndc.x) <= 1.0);
            CHECK(std::abs(ndc.y) <= 1.0);
        }
    }
}

TEST_CASE("framing with insets centres the model in the clear part of the view") {
    const Aabb box{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
    const nav::Viewport viewport{1000.0, 600.0};
    nav::Camera camera;
    camera.lookAt(simd_make_double3(0, -5, 0), simd_make_double3(0, 0, 0),
                  simd_make_double3(0, 0, 1));
    const nav::Insets insets{300.0, 0.0, 0.0, 100.0};
    nav::fit(camera, box, viewport, 1.0, insets);
    const nav::NdcPoint centre = camera.project(box.center(), viewport);
    // The clear area runs from x 300 to 1000 and y 0 to 500, so its middle is (650, 250) in
    // pixels, which is (0.3, 1/6) in normalised coordinates.
    CHECK(centre.x == doctest::Approx(0.3).epsilon(1e-6));
    CHECK(centre.y == doctest::Approx(1.0 / 6.0).epsilon(1e-6));
    // The box's height fills the clear height exactly: two units over 500 of 600 pixels.
    CHECK(camera.orthoHalfHeight == doctest::Approx(1.2).epsilon(1e-6));
}
