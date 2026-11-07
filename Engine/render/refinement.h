// Decides when geometry is too coarse for the view. A definition's chord error shrinks by half per
// level, so the level a view needs follows from the error in pixels at its nearest visible
// occurrence.
#pragma once

#include "base/types.h"
#include "math/geometry.h"
#include "nav/camera.h"

#include <span>
#include <vector>

namespace geo::render {

struct RefinementPolicy {
    double linearDeflection = 0.0; // chord error at level 0, in world units
    u32 maxLevel = 0;
    double maxErrorPixels = 0.5;
};

/// A placed definition, by its bounding sphere.
struct PlacedExtent {
    Vec3 center;
    double radius;
    u32 definition;
};

/// The level each definition needs for its visible occurrences, or zero if it is not visible.
std::vector<u32> neededLevels(std::span<const PlacedExtent> placed, usize definitionCount,
                              const nav::Camera& camera, const nav::Viewport& viewport,
                              const RefinementPolicy& policy);

} // namespace geo::render
