#include "nav/flight.h"

#include <cmath>
#include <doctest/doctest.h>

using namespace geo;
using namespace geo::nav;

TEST_CASE("the easing curve starts slow to rest and ends at one") {
    CHECK(ease(0.0) == doctest::Approx(0.0));
    CHECK(ease(1.0) == doctest::Approx(1.0));
    // cubic-bezier(0.3, 0.7, 0.2, 1) is front-loaded: well past halfway at the midpoint.
    CHECK(ease(0.5) > 0.75);
    double previous = 0.0;
    for (int i = 1; i <= 20; ++i) {
        const double value = ease(i / 20.0);
        CHECK(value >= previous);
        previous = value;
    }
}

TEST_CASE("a flight ends exactly at its destination and keeps the target on the way") {
    const Aabb box{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
    const Viewport viewport{800, 600};
    Camera start;
    start.lookAt(simd_make_double3(5, -5, 5), simd_make_double3(0, 0, 0),
                 simd_make_double3(0, 0, 1));
    fit(start, box, viewport, 1.1);
    const Camera top = standardView(start, StandardView::Top, box, viewport, 1.1);

    Flight flight;
    flight.start(start, top, 10.0, 0.5);
    const Camera middle = flight.at(10.25);
    CHECK(flight.active());
    CHECK(simd_length(middle.target()) < 1e-9);
    const Camera end = flight.at(10.6);
    CHECK_FALSE(flight.active());
    CHECK(simd_length(end.position - top.position) < 1e-9);
    CHECK(simd_dot(end.forward(), simd_make_double3(0, 0, -1)) == doctest::Approx(1.0));
}

TEST_CASE("standard views look from the named side with Z up") {
    const Aabb box{simd_make_double3(0, 0, 0), simd_make_double3(2, 2, 2)};
    const Viewport viewport{800, 600};
    const Camera camera;
    const Camera front = standardView(camera, StandardView::Front, box, viewport, 1.1);
    CHECK(simd_dot(front.forward(), simd_make_double3(0, 1, 0)) == doctest::Approx(1.0));
    CHECK(simd_dot(front.up(), simd_make_double3(0, 0, 1)) == doctest::Approx(1.0));
    const Camera right = standardView(camera, StandardView::Right, box, viewport, 1.1);
    CHECK(simd_dot(right.forward(), simd_make_double3(-1, 0, 0)) == doctest::Approx(1.0));
}
