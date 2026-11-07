// The camera: one pose, in double precision, plus a projection.
#pragma once

#include "math/geometry.h"

namespace geo::nav {

struct Viewport {
    double width = 1;
    double height = 1;

    double aspect() const { return width / height; }
};

/// A point in normalised device coordinates, with x right and y up, both in [-1, 1] on screen.
struct NdcPoint {
    double x, y;
    double viewDepth; // distance in front of the camera along its forward axis
};

struct Camera {
    Vec3 position = simd_make_double3(0.0, 0.0, 1.0);
    simd_quatd orientation = simd_quaternion(0.0, 0.0, 0.0, 1.0);
    double focusDistance = 1.0;       // distance to the target, which rotation orbits by default
    double orthoHalfHeight = 1.0;     // half the view height, in world units
    double fovY = 0.5235987755982988; // 30 degrees
    bool perspective = false;

    // Camera axes in world space. The camera looks along -z of its own frame.
    Vec3 right() const { return simd_act(orientation, simd_make_double3(1.0, 0.0, 0.0)); }
    Vec3 up() const { return simd_act(orientation, simd_make_double3(0.0, 1.0, 0.0)); }
    Vec3 back() const { return simd_act(orientation, simd_make_double3(0.0, 0.0, 1.0)); }
    Vec3 forward() const { return -back(); }
    Vec3 target() const { return position + forward() * focusDistance; }

    /// Half the view height at a given depth in front of the camera.
    double halfHeightAt(double depth) const;

    NdcPoint project(Vec3 point, const Viewport& viewport) const;

    /// The world point at an NDC position and depth.
    Vec3 unproject(double ndcX, double ndcY, double depth, const Viewport& viewport) const;

    /// Points the camera from `eye` at `target`, keeping `up` as close to vertical as possible.
    void lookAt(Vec3 eye, Vec3 target, Vec3 up);
};

/// Rotates the camera about `pivot`, like a trackball.
///
/// `dxN` and `dyN` are the drag in trackball units: pixels times 2 / viewport width, with y
/// negated, so a full-width drag is 2 units. The axis is built from the view, so a drag feels the
/// same wherever the pivot is, and because position and orientation turn together, panned framing
/// is preserved.
void tumble(Camera& camera, Vec3 pivot, double dxN, double dyN, double rotateSpeed);

/// Scales the view by `factor` (below 1 zooms in) while keeping `anchor`, shown at (`ndcX`,
/// `ndcY`), on that same pixel.
void zoomAbout(Camera& camera, double factor, Vec3 anchor, double ndcX, double ndcY,
               const Viewport& viewport);

/// Moves the camera so the scene follows the pointer exactly at `depth`.
void pan(Camera& camera, double pixelsX, double pixelsY, double depth, const Viewport& viewport);

/// Parts of the viewport covered by chrome, in pixels. Framing keeps the model in what is left.
struct Insets {
    double left = 0, top = 0, right = 0, bottom = 0;
};

/// Frames `bounds` with `margin` around it, keeping the view direction, in the part of the
/// viewport that `insets` leaves clear.
void fit(Camera& camera, const Aabb& bounds, const Viewport& viewport, double margin,
         const Insets& insets = {});

} // namespace geo::nav
