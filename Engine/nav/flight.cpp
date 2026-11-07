#include "nav/flight.h"

#include <algorithm>
#include <cmath>

namespace geo::nav {
namespace {

constexpr double kIsoAzimuth = 35.0 * M_PI / 180.0;
constexpr double kIsoElevation = 25.0 * M_PI / 180.0;

// One coordinate of a cubic Bezier from (0,0) to (1,1) with control points a and b.
double bezier(double t, double a, double b) {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

double bezierSlope(double t, double a, double b) {
    const double u = 1.0 - t;
    return 3.0 * u * u * a + 6.0 * u * t * (b - a) + 3.0 * t * t * (1.0 - b);
}

} // namespace

double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    // Find the curve parameter whose x is t, by Newton's method, then read its y.
    constexpr double x1 = 0.3, y1 = 0.7, x2 = 0.2, y2 = 1.0;
    double s = t;
    for (int i = 0; i < 8; ++i) {
        const double slope = bezierSlope(s, x1, x2);
        if (std::fabs(slope) < 1e-9) {
            break;
        }
        s = std::clamp(s - (bezier(s, x1, x2) - t) / slope, 0.0, 1.0);
    }
    return bezier(s, y1, y2);
}

void Flight::start(const Camera& from, const Camera& to, double now, double seconds) {
    from_ = from;
    to_ = to;
    start_ = now;
    duration_ = seconds;
    active_ = true;
}

Camera Flight::at(double now) {
    if (!active_) {
        return to_;
    }
    const double t = duration_ > 0.0 ? (now - start_) / duration_ : 1.0;
    if (t >= 1.0) {
        active_ = false;
        return to_;
    }
    const double k = ease(t);
    Camera camera = to_;
    camera.orientation = simd_slerp(from_.orientation, to_.orientation, k);
    const Vec3 target = simd_mix(from_.target(), to_.target(), simd_double3(k));
    // Distances change on a log scale, so zooming far in or out moves at an even pace.
    const auto logMix = [&](double a, double b) {
        return std::exp(std::log(std::max(a, 1e-12)) * (1.0 - k) +
                        std::log(std::max(b, 1e-12)) * k);
    };
    camera.focusDistance = logMix(from_.focusDistance, to_.focusDistance);
    camera.orthoHalfHeight = logMix(from_.orthoHalfHeight, to_.orthoHalfHeight);
    camera.position = target - camera.forward() * camera.focusDistance;
    return camera;
}

Camera standardView(const Camera& current, StandardView view, const Aabb& bounds,
                    const Viewport& viewport, double margin) {
    Vec3 from{0, -1, 0}, up{0, 0, 1};
    switch (view) {
    case StandardView::Front:
        break;
    case StandardView::Back:
        from = {0, 1, 0};
        break;
    case StandardView::Left:
        from = {-1, 0, 0};
        break;
    case StandardView::Right:
        from = {1, 0, 0};
        break;
    case StandardView::Top:
        from = {0, 0, 1};
        up = {0, 1, 0};
        break;
    case StandardView::Bottom:
        from = {0, 0, -1};
        up = {0, -1, 0};
        break;
    case StandardView::Iso:
        from = {std::cos(kIsoElevation) * std::sin(kIsoAzimuth),
                -std::cos(kIsoElevation) * std::cos(kIsoAzimuth), std::sin(kIsoElevation)};
        break;
    }
    Camera camera = current;
    camera.lookAt(bounds.center() + from, bounds.center(), up);
    fit(camera, bounds, viewport, margin);
    return camera;
}

} // namespace geo::nav
