// Chooses the point the camera rotates around.
//
// Far away, rotation should turn the whole model about its centre. Close up, that centre may be far
// off screen and the model swings out of view, so the pivot moves to a surface near the middle of the
// view. Hysteresis stops it flipping between the two near the threshold.
#pragma once

#include "math/geometry.h"
#include "nav/camera.h"

#include <functional>
#include <optional>

namespace geo::nav {

enum class PivotMode {
    Adaptive, // visible centre, or a nearby surface when close up
    Visible,  // always the centre of what is visible
    Pointer,  // always the surface under the pointer
};

/// Finds the surface at an NDC position, if any.
using SurfaceProbe = std::function<std::optional<Vec3>(double ndcX, double ndcY)>;

class OrbitPolicy {
public:
    void setMode(PivotMode mode);
    PivotMode mode() const { return mode_; }

    /// Fixes the pivot until cleared, overriding the mode.
    void setExplicitPivot(Vec3 pivot);
    void clearExplicitPivot();

    /// Updates what is visible. The pivot re-centres, because a surface it sat on may be gone.
    void setVisibleBounds(const Aabb& bounds);

    /// Marks that the view changed by something other than rotation. Successive rotations keep their
    /// pivot, so only pan, zoom and fit call this.
    void navigated() { evaluate_ = true; }

    /// Returns the pivot for a new drag. `pointerNdc` is used only in Pointer mode: adaptive mode
    /// samples around the viewport centre.
    Vec3 resolve(const Camera& camera, const Viewport& viewport, double pointerX, double pointerY,
                 const SurfaceProbe& probe);

    Vec3 pivot() const { return pivot_; }
    bool closeUp() const { return closeUp_; }

private:
    void updateCloseUp(const Camera& camera, const Viewport& viewport);

    PivotMode mode_ = PivotMode::Adaptive;
    Aabb bounds_{};
    Vec3 pivot_{};
    bool hasExplicitPivot_ = false;
    bool closeUp_ = false;
    bool evaluate_ = true;
};

} // namespace geo::nav
