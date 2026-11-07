#include "nav/camera.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace geo::nav {

double Camera::halfHeightAt(double depth) const {
    return perspective ? depth * std::tan(fovY * 0.5) : orthoHalfHeight;
}

NdcPoint Camera::project(Vec3 point, const Viewport& viewport) const {
    const Vec3 offset = point - position;
    const double depth = simd_dot(offset, forward());
    const double halfHeight = halfHeightAt(depth);
    return {simd_dot(offset, right()) / (halfHeight * viewport.aspect()),
            simd_dot(offset, up()) / halfHeight, depth};
}

Vec3 Camera::unproject(double ndcX, double ndcY, double depth, const Viewport& viewport) const {
    const double halfHeight = halfHeightAt(depth);
    return position + forward() * depth + right() * (ndcX * halfHeight * viewport.aspect()) +
           up() * (ndcY * halfHeight);
}

void Camera::lookAt(Vec3 eye, Vec3 target, Vec3 worldUp) {
    position = eye;
    focusDistance = simd_length(target - eye);
    const Vec3 z = simd_normalize(eye - target);
    Vec3 x = simd_cross(worldUp, z);
    if (simd_length_squared(x) < 1e-12) {
        // Looking straight along the up vector, so any perpendicular axis will do.
        x = simd_cross(simd_make_double3(1.0, 0.0, 0.0), z);
        if (simd_length_squared(x) < 1e-12) {
            x = simd_cross(simd_make_double3(0.0, 1.0, 0.0), z);
        }
    }
    x = simd_normalize(x);
    const Vec3 y = simd_cross(z, x);
    orientation = simd_quaternion(simd_matrix(x, y, z));
}

void tumble(Camera& camera, Vec3 pivot, double dxN, double dyN, double rotateSpeed) {
    const double angle = std::hypot(dxN, dyN);
    if (angle == 0.0) {
        return;
    }
    const Vec3 eye = camera.back() * camera.focusDistance;
    const Vec3 moveDirection = camera.up() * dyN + camera.right() * dxN;
    const Vec3 axis = simd_normalize(simd_cross(moveDirection, eye));
    const simd_quatd rotation = simd_quaternion(angle * rotateSpeed, axis);
    camera.position = simd_act(rotation, camera.position - pivot) + pivot;
    camera.orientation = simd_normalize(simd_mul(rotation, camera.orientation));
}

void zoomAbout(Camera& camera, double factor, Vec3 anchor, double ndcX, double ndcY,
               const Viewport& viewport) {
    if (camera.perspective) {
        // Moving along the anchor's ray keeps it on the same pixel. Focus scales too, so dollying
        // in never stalls at the old target.
        camera.position += (anchor - camera.position) * (1.0 - factor);
        camera.focusDistance *= factor;
        return;
    }
    const double oldHalfHeight = camera.orthoHalfHeight;
    camera.orthoHalfHeight *= factor;
    const double shrink = oldHalfHeight - camera.orthoHalfHeight;
    camera.position +=
        camera.right() * (ndcX * viewport.aspect() * shrink) + camera.up() * (ndcY * shrink);
}

void pan(Camera& camera, double pixelsX, double pixelsY, double depth, const Viewport& viewport) {
    const double worldPerPixel = 2.0 * camera.halfHeightAt(depth) / viewport.height;
    camera.position -= camera.right() * (pixelsX * worldPerPixel);
    camera.position += camera.up() * (pixelsY * worldPerPixel);
}

void fit(Camera& camera, const Aabb& bounds, const Viewport& viewport, double margin,
         const Insets& insets) {
    // Fit into the clear area, but never shrink it below a third of the view, so a small window
    // full of chrome still shows the model.
    const Viewport clear{
        std::max(viewport.width - insets.left - insets.right, viewport.width / 3),
        std::max(viewport.height - insets.top - insets.bottom, viewport.height / 3)};
    const Vec3 center = bounds.center();
    const Vec3 half = (bounds.max - bounds.min) * 0.5;
    const Vec3 right = camera.right(), up = camera.up(), forward = camera.forward();

    // The box's extent along each camera axis.
    const auto extentAlong = [&](Vec3 axis) {
        return std::abs(axis.x) * half.x + std::abs(axis.y) * half.y + std::abs(axis.z) * half.z;
    };
    const double extentRight = extentAlong(right);
    const double extentUp = extentAlong(up);
    const double extentDepth = extentAlong(forward);
    // The half height that fits the clear area, scaled up to the whole viewport.
    const double clearHalfHeight =
        std::max({extentUp, extentRight / clear.aspect(), 1e-9}) * margin;
    const double halfHeight = clearHalfHeight * viewport.height / clear.height;

    camera.orthoHalfHeight = halfHeight;
    const double distance = camera.perspective
                                ? halfHeight / std::tan(camera.fovY * 0.5) + extentDepth
                                : extentDepth + halfHeight;
    camera.focusDistance = distance;
    // Shift sideways so the model's centre lands in the middle of the clear area.
    const double worldPerPixel = 2.0 * halfHeight / viewport.height;
    const double shiftRight = (insets.left - insets.right) * 0.5 * worldPerPixel;
    const double shiftUp = (insets.bottom - insets.top) * 0.5 * worldPerPixel;
    camera.position = center - forward * distance - right * shiftRight - up * shiftUp;
}

} // namespace geo::nav
