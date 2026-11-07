// The sheet behind the model: a vertical wash, an optional vignette and drafting grid, and paper
// grain. Drawn at the far plane after opaque surfaces, so it shades only what they leave bare.
#include "noise.h"
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

struct SheetVertex {
    float4 position [[position]];
};

// One triangle covering the viewport, at the far plane, which is depth 0 with reversed depth.
vertex SheetVertex sheetVertex(uint vertexId [[vertex_id]]) {
    const float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    return {float4(p * 2.0 - 1.0, 0.0, 1.0)};
}

// The shadow at a pixel, from the half-resolution mask: a crisp tent grown a little, so hairline
// gaps between parts that touch close, or for soft styles a disc of samples.
static float shadowAt(float2 px, texture2d<float> mask, constant StyleUniforms& style) {
    constexpr sampler linear(filter::linear, address::clamp_to_edge, coord::pixel);
    const float2 at = px * 0.5;
    if (style.shadowSoft <= 1.0) {
        float shade = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
                shade += w * mask.sample(linear, at + float2(x, y) * 0.5).r;
            }
        }
        return saturate(shade / 16.0 * 1.25);
    }
    // A golden-angle spiral, weighted toward the middle.
    float shade = 0.0, total = 0.0;
    const float radius = style.shadowSoft * 0.5;
    for (int i = 0; i < 24; ++i) {
        const float r = sqrt((float(i) + 0.5) / 24.0);
        const float a = float(i) * 2.39996;
        const float w = 1.0 - 0.6 * r;
        shade += w * mask.sample(linear, at + float2(cos(a), sin(a)) * r * radius).r;
        total += w;
    }
    return shade / total;
}

fragment float4 sheetFragment(SheetVertex in [[stage_in]],
                              constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                              constant StyleUniforms& style [[buffer(kBufferStyle)]],
                              texture2d<float> shadow [[texture(kTextureShadow)]]) {
    const float2 px = in.position.xy;
    const float2 uv = px / frame.viewport; // y runs down
    float3 color = mix(style.sheetTop, style.sheetBottom, uv.y);
    if (style.vignette > 0.0) {
        const float r = length((uv - 0.5) * float2(frame.viewport.x / frame.viewport.y, 1.0));
        color *= 1.0 - style.vignette * smoothstep(0.3, 1.0, r);
    }
    if (style.gridAlpha > 0.0 && style.gridPitch > 0.0) {
        // Minor lines every pitch and heavier major lines every five, a pixel wide at any scale.
        const float2 cell = px / style.gridPitch;
        const float2 minor = abs(fract(cell - 0.5) - 0.5) / fwidth(cell);
        const float2 major = abs(fract(cell / 5.0 - 0.5) - 0.5) / fwidth(cell / 5.0);
        const float line = max(1.0 - min(min(minor.x, minor.y), 1.0),
                               1.6 * (1.0 - min(min(major.x, major.y), 1.0)));
        color = mix(color, style.gridColor, style.gridAlpha * line);
    }
    // Paper tooth: a fine grain over a coarser one.
    const float fine = valueNoise(px / (1.35 * style.pixelsPerPoint));
    const float paper = 0.55 * fine + 0.45 * valueNoise(px / (4.0 * style.pixelsPerPoint));
    color = mix(color, style.grainTint, 0.11 * style.grain * paper);
    if (style.shadowOn != 0 && frame.shadowVisibility > 0.0) {
        color = mix(color, style.shadowColor,
                    style.shadowAlpha * frame.shadowVisibility * shadowAt(px, shadow, style));
    }
    return float4(color, 1.0);
}
