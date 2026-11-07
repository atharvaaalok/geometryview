// Small geometric types shared by the engine, on Apple's simd library.
#pragma once

#include "base/simd.h"

namespace geo {

using Vec3 = simd_double3;

struct Aabb {
    Vec3 min;
    Vec3 max;

    Vec3 center() const { return (min + max) * 0.5; }
    double radius() const { return simd_length(max - min) * 0.5; }
};

} // namespace geo
