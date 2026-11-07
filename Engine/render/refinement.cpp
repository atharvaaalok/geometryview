#include "render/refinement.h"

#include <algorithm>
#include <cmath>

namespace geo::render {
namespace {

constexpr double kNearestShare = 0.05;

} // namespace

std::vector<u32> neededLevels(std::span<const PlacedExtent> placed, usize definitionCount,
                              const nav::Camera& camera, const nav::Viewport& viewport,
                              const RefinementPolicy& policy) {
    std::vector<u32> levels(definitionCount, 0);
    if (policy.linearDeflection <= 0.0 || policy.maxLevel == 0) {
        return levels;
    }
    const Vec3 forward = camera.forward(), right = camera.right(), up = camera.up();
    const double aspect = viewport.aspect();
    for (const PlacedExtent& extent : placed) {
        if (extent.definition >= definitionCount) {
            continue;
        }
        const Vec3 offset = extent.center - camera.position;
        const double depth = simd_dot(offset, forward);
        if (depth < -extent.radius) {
            continue; // behind the camera
        }
        // The nearest depth any of it can have, which is where its error looks largest. With the
        // camera inside the sphere that is not zero, but a small share of the sphere.
        const double nearest = std::max(depth - extent.radius, extent.radius * kNearestShare);
        const double halfHeight = camera.halfHeightAt(std::max(depth, nearest));
        if (std::fabs(simd_dot(offset, up)) > halfHeight + extent.radius ||
            std::fabs(simd_dot(offset, right)) > halfHeight * aspect + extent.radius) {
            continue; // outside the view
        }
        const double pixelsPerUnit = viewport.height / (2.0 * camera.halfHeightAt(nearest));
        const double error = policy.linearDeflection * pixelsPerUnit / policy.maxErrorPixels;
        if (error <= 1.0) {
            continue;
        }
        const u32 level = u32(std::min<double>(std::ceil(std::log2(error)), policy.maxLevel));
        levels[extent.definition] = std::max(levels[extent.definition], level);
    }
    return levels;
}

} // namespace geo::render
