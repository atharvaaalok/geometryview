// The integer grid a definition's positions are stored on. Shaders decode a grid point as
// `grid * scale + lo` in float, so a point found on the grid here is exactly the point drawn. See
// ADR 0004.
#pragma once

#include "base/types.h"

#include <array>
#include "base/simd.h"

namespace geo::import {

struct VertexGrid {
    simd_float3 lo;     // the definition's lower bounds
    simd_float3 toGrid; // definition space to grid units
    simd_float3 scale;  // grid units to definition space
    simd_float3 max;    // the largest grid coordinate on each axis
    bool wide = false;  // 24 bits per axis rather than 16

    /// The grid point nearest `p`. Clamped because at 24 bits a float cannot hold the top grid step
    /// exactly, and a point at the maximum could round past it and wrap to zero.
    std::array<u32, 3> of(simd_float3 p) const {
        const simd_float3 g = simd_clamp(simd::floor((p - lo) * toGrid + 0.5f), 0.0f, max);
        return {u32(g.x), u32(g.y), u32(g.z)};
    }

    simd_float3 decode(const std::array<u32, 3>& g) const {
        return simd_make_float3(float(g[0]), float(g[1]), float(g[2])) * scale + lo;
    }
};

} // namespace geo::import
