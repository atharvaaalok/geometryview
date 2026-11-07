// The sheet behind the model, shared by the sheet itself and the see-through shell's far walls,
// which mix it in.
#pragma once

#include "noise.h"
#include "shared/gpu_types.h"
#include <metal_stdlib>

// A vertical wash, an optional vignette and drafting grid, and paper grain.
static float3 sheetColor(float2 px, constant FrameUniforms& frame, constant StyleUniforms& style) {
    const float2 uv = px / frame.viewport; // y runs down
    float3 color = metal::mix(style.sheetTop, style.sheetBottom, uv.y);
    if (style.vignette > 0.0) {
        const float r =
            metal::length((uv - 0.5) * float2(frame.viewport.x / frame.viewport.y, 1.0));
        color *= 1.0 - style.vignette * metal::smoothstep(0.3, 1.0, r);
    }
    if (style.gridAlpha > 0.0 && style.gridPitch > 0.0) {
        // Minor lines every pitch and heavier major lines every five, a pixel wide at any scale.
        const float2 cell = px / style.gridPitch;
        const float2 minor = metal::abs(metal::fract(cell - 0.5) - 0.5) / metal::fwidth(cell);
        const float2 major =
            metal::abs(metal::fract(cell / 5.0 - 0.5) - 0.5) / metal::fwidth(cell / 5.0);
        const float line = metal::max(1.0 - metal::min(metal::min(minor.x, minor.y), 1.0),
                                      1.6 * (1.0 - metal::min(metal::min(major.x, major.y), 1.0)));
        color = metal::mix(color, style.gridColor, style.gridAlpha * line);
    }
    // Paper tooth: a fine grain over a coarser one.
    const float fine = valueNoise(px / (1.35 * style.pixelsPerPoint));
    const float paper = 0.55 * fine + 0.45 * valueNoise(px / (4.0 * style.pixelsPerPoint));
    color = metal::mix(color, style.grainTint, 0.11 * style.grain * paper);
    return color;
}
