// Decoding and culling shared by every pass that draws a definition's geometry.
#pragma once

#include "shared/gpu_types.h"
#include <metal_stdlib>

static inline float3 transformPoint(const device GpuOccurrence& occurrence, float3 p) {
    const float4 h = float4(p, 1.0);
    return float3(metal::dot(occurrence.rows[0], h), metal::dot(occurrence.rows[1], h),
                  metal::dot(occurrence.rows[2], h));
}

static inline float3 transformDirection(const device GpuOccurrence& occurrence, float3 v) {
    return float3(metal::dot(occurrence.rows[0].xyz, v), metal::dot(occurrence.rows[1].xyz, v),
                  metal::dot(occurrence.rows[2].xyz, v));
}

static inline float3 decodeOctahedral(short2 encoded) {
    const float2 f = float2(encoded) / 32767.0;
    float3 n = float3(f, 1.0 - metal::abs(f.x) - metal::abs(f.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - metal::abs(n.yx)) * metal::sign(n.xy);
    }
    return metal::normalize(n);
}

// A vertex's position in definition space. Integers first, so a vertex at the same grid point
// decodes to the same float wherever it is read, which keeps shared edges watertight and puts
// feature edges exactly on their surfaces. See ADR 0004.
static inline float3 decodePosition(const device GpuDefinition& definition, uint vertexIndex) {
    const device PackedVertex& v = definition.vertices[vertexIndex];
    uint3 grid = uint3(v.x, v.y, v.z);
    if ((definition.flags & kDefinitionWide) != 0) {
        const uint high = definition.highBits[vertexIndex];
        grid |= uint3(high & 0xFF, (high >> 8) & 0xFF, (high >> 16) & 0xFF) << 16;
    }
    return float3(grid) * definition.decodeScale + definition.boundsMin;
}

// A sphere's centre in world space, from bounds stored on the definition's 16-bit bounds grid.
static inline float3 boundsCenter(const device GpuDefinition& definition,
                                  const device GpuOccurrence& occurrence, ushort3 gridCenter) {
    return transformPoint(occurrence,
                          float3(gridCenter) * definition.boundsScale + definition.boundsMin);
}

// Whether a sphere can touch the view.
static inline bool sphereInView(float3 center, float radius, constant FrameUniforms& frame) {
    const float3 offset = center - frame.eye;
    const float x = metal::abs(metal::dot(offset, frame.right));
    const float y = metal::abs(metal::dot(offset, frame.up));
    const float depth = metal::dot(offset, frame.forward);
    if (frame.perspective != 0) {
        return depth >= -radius && x <= depth * frame.tanHalfX + radius * frame.secHalfX &&
               y <= depth * frame.tanHalfY + radius * frame.secHalfY;
    }
    return x <= frame.halfWidth + radius && y <= frame.halfHeight + radius;
}

// The flags a face shows: its occurrence's own, combined with any of its own.
static inline uint faceFlags(OccurrenceState state, const device uchar* faceStates, uint face) {
    return (state.flags & (kStateHidden | kStateGhost | kStateSelected)) |
           uint(faceStates[state.facePage + face]);
}
