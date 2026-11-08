// Ambient occlusion from depth, at half resolution: how much of the open sky above each point
// nearby geometry hides, so creases, gaps between parts and the feet of walls darken as they do
// under a studio light. After step_viewer's.
//
// Positions come back from depth. The normal comes from whichever neighbour on each axis lies on
// the same surface, so silhouettes do not smear it. Samples lie on a spiral, turned per pixel,
// within a world radius; the composite blurs the result while keeping to one surface.
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

static inline float viewDepth(float depth, constant FrameUniforms& frame) {
    // A precise division: fast math's approximate one leaves surfaces seen at a grazing angle
    // rough enough, in perspective, for the occlusion to darken them in speckles.
    return frame.perspective != 0 ? metal::precise::divide(frame.depthNear, max(depth, 1e-12))
                                  : frame.depthFar - depth * (frame.depthFar - frame.depthNear);
}

// A pixel's position in view space, looking down -z.
static inline float3 viewPosition(int2 p, float depth, constant FrameUniforms& frame) {
    const float2 ndc = float2((float(p.x) + 0.5) / frame.viewport.x * 2.0 - 1.0,
                              1.0 - (float(p.y) + 0.5) / frame.viewport.y * 2.0);
    const float z = viewDepth(depth, frame);
    const float2 xy = frame.perspective != 0 ? ndc * float2(frame.tanHalfX, frame.tanHalfY) * z
                                             : ndc * float2(frame.halfWidth, frame.halfHeight);
    return float3(xy, -z);
}

kernel void ambientOcclusion(texture2d<float> depths [[texture(kTexturePickDepths)]],
                             texture2d<uint> tags [[texture(kTextureTags)]],
                             texture2d<float, access::write> occlusion [[texture(kTextureAo)]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             constant StyleUniforms& style [[buffer(kBufferStyle)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= occlusion.get_width() || gid.y >= occlusion.get_height()) {
        return;
    }
    const int2 last = int2(depths.get_width(), depths.get_height()) - 1;
    const auto depthAt = [&](int2 q) { return depths.read(uint2(clamp(q, int2(0), last))).r; };

    const int2 p = int2(gid) * 2;
    const float d = depthAt(p);
    const uint tag = tags.read(uint2(clamp(p, int2(0), last))).r;
    if (d <= 0.0 || tag == 0 || (tag & kTagCap) != 0) {
        occlusion.write(float4(1.0), gid); // the sheet, only a ghost, or a section cap
        return;
    }
    const float3 center = viewPosition(p, d, frame);
    // The neighbour on an axis that lies on the same surface is the one nearer in depth.
    const auto tangent = [&](int2 axis) {
        const float da = depthAt(p + axis), db = depthAt(p - axis);
        const float3 a = viewPosition(p + axis, da, frame) - center;
        const float3 b = center - viewPosition(p - axis, db, frame);
        if (da <= 0.0) {
            return b;
        }
        if (db <= 0.0) {
            return a;
        }
        return abs(a.z) < abs(b.z) ? a : b;
    };
    float3 normal = normalize(cross(tangent(int2(1, 0)), tangent(int2(0, -1))));
    if (normal.z < 0.0) {
        normal = -normal; // toward the eye
    }

    const float pixelWorld =
        frame.perspective != 0 ? frame.worldPerPixel * -center.z : frame.worldPerPixel;
    const float px = style.pixelsPerPoint;
    const float reach = clamp(frame.aoRadius / pixelWorld, 4.0 * px, 48.0 * px);
    const float radius = reach * pixelWorld;
    // Interleaved gradient noise turns the spiral per pixel; the composite's blur averages it.
    const float turn =
        6.2831853 *
        fract(52.9829189 * fract(dot(float2(gid) + 0.5, float2(0.06711056, 0.00583715))));
    const uint samples = frame.aoSamples;
    float occluded = 0.0;
    for (uint i = 0; i < samples; ++i) {
        const float t = (float(i) + 0.5) / float(samples);
        const float angle = turn + float(i) * 2.3999632;
        const int2 q = p + int2(round(float2(cos(angle), sin(angle)) * t * reach));
        const float dq = depthAt(q);
        if (dq <= 0.0) {
            continue;
        }
        const float3 v = viewPosition(q, dq, frame) - center;
        const float length = metal::length(v);
        const float falloff = saturate(1.0 - (length * length) / (radius * radius));
        occluded += max(dot(normal, v) / max(length, 1e-6) - 0.05, 0.0) * falloff;
    }
    occlusion.write(float4(pow(saturate(1.0 - 2.6 * occluded / float(samples)), 1.4)), gid);
}
