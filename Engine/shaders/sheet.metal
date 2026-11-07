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

fragment float4 sheetFragment(SheetVertex in [[stage_in]],
                              constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                              constant StyleUniforms& style [[buffer(kBufferStyle)]]) {
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
    return float4(color, 1.0);
}
