// Animated camera moves, such as to a standard view. A flight turns about the target and changes
// distance on a log scale, so it feels the same whether the model is a screw or a ship.
#pragma once

#include "nav/camera.h"

namespace geo::nav {

/// The interface's easing curve, cubic-bezier(0.3, 0.7, 0.2, 1), so camera and chrome move alike.
double ease(double t);

class Flight {
public:
    /// Starts a flight from `from` to `to` lasting `seconds`. A duration of zero jumps.
    void start(const Camera& from, const Camera& to, double now, double seconds);
    bool active() const { return active_; }
    void cancel() { active_ = false; }

    /// The camera at `now`. The last call, at or after the end, returns `to` exactly and ends the
    /// flight.
    Camera at(double now);

private:
    Camera from_, to_;
    double start_ = 0.0, duration_ = 0.0;
    bool active_ = false;
};

/// Standard views, named for where the camera looks from, with Z up.
enum class StandardView { Front, Back, Left, Right, Top, Bottom, Iso };

/// The camera looking at `bounds` from `view`, fitted, keeping the projection.
Camera standardView(const Camera& current, StandardView view, const Aabb& bounds,
                    const Viewport& viewport, double margin, const Insets& insets = {});

} // namespace geo::nav
