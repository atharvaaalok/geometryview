// The final touches over the resolved image, in one full-screen draw: outlines from the bodies'
// tags and depth, and print texture on the figure. After step_viewer's composite.
//
// Outlines are heavy where a body meets the background, lighter between bodies, and where a body
// folds in front of itself, a step in depth within one body. Ghosts get a faint outline of their
// own. Everything is sampled through the hand-drawn wobble, so outlines wobble with the edges.
//
// The result blends as ink + colour * alpha, so outlines go over the colour, and print texture
// darkens both.
#include "noise.h"
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

constant float2 kDirections[8] = {
    float2(1.0, 0.0),  float2(0.7071, 0.7071),   float2(0.0, 1.0),  float2(-0.7071, 0.7071),
    float2(-1.0, 0.0), float2(-0.7071, -0.7071), float2(0.0, -1.0), float2(0.7071, -0.7071)};

static inline float2 wobbleOffset(float2 pixel, constant StyleUniforms& style) {
    if (style.wobble <= 0.0) {
        return float2(0.0);
    }
    const float2 q = pixel / style.wobblePeriod;
    return style.wobble * 2.0 *
           float2(valueNoise(q) - 0.5, valueNoise(q + float2(17.0, 9.0)) - 0.5);
}

// Distance along the view, from stored depth, which is reversed.
static inline float viewDepth(float depth, constant FrameUniforms& frame) {
    return frame.perspective != 0 ? frame.depthNear / max(depth, 1e-12)
                                  : frame.depthFar - depth * (frame.depthFar - frame.depthNear);
}

static inline float pixelWorld(float z, constant FrameUniforms& frame) {
    return frame.perspective != 0 ? frame.worldPerPixel * z : frame.worldPerPixel;
}

// Coverage of a line `width` pixels wide whose middle is `distance` pixels away.
static inline float lineCover(float width, float distance) {
    return 1.0 - smoothstep(0.5 * width - 0.5, 0.5 * width + 0.5, distance);
}

fragment float4 compositeFragment(float4 position [[position]],
                                  constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                  constant StyleUniforms& style [[buffer(kBufferStyle)]],
                                  texture2d<float> depths [[texture(kTexturePickDepths)]],
                                  texture2d<uint> tags [[texture(kTextureTags)]]) {
    const float2 px = position.xy;
    const int2 last = int2(tags.get_width(), tags.get_height()) - 1;
    const auto at = [&](int2 q) { return uint2(clamp(q, int2(0), last)); };
    const int2 p = int2(px + wobbleOffset(px, style));
    const uint2 c = tags.read(at(p)).rg;

    // Search outward for a change of body, solid and ghost alike.
    float solid = 0.0, ghost = 0.0;
    const float reach = 0.5 * max(style.silhouette, style.partLine) + 1.0;
    for (float r = 1.0; r <= reach; r += 1.0) {
        for (uint k = 0; k < 8; ++k) {
            const int2 o = int2(round(kDirections[k] * r));
            const uint2 n = tags.read(at(p + o)).rg;
            const float distance = length(float2(o)) - 0.5;
            // Against the background a line is as heavy as the silhouette; between bodies, the
            // lighter part line.
            if (n.r != c.r) {
                const bool background = c.r == 0 || n.r == 0;
                solid =
                    max(solid, lineCover(background ? style.silhouette : style.partLine, distance));
            }
            if (n.g != c.g) {
                const bool background = c.g == 0 || n.g == 0;
                ghost =
                    max(ghost, lineCover(background ? style.silhouette : style.partLine, distance));
            }
        }
    }

    // A body folding in front of itself, such as a boss before its own plate: a step in depth
    // within one body. A step, not a kink: the second difference must be large in world terms and
    // large against the slope, so a curved surface seen edge on does not ink its facets.
    if (c.r != 0 && solid < 1.0) {
        const int s = max(1, int(round(style.pixelsPerPoint)));
        const int2 left = p - int2(s, 0), right = p + int2(s, 0);
        const int2 down = p + int2(0, s), up = p - int2(0, s);
        const bool same = tags.read(at(left)).r == c.r && tags.read(at(right)).r == c.r &&
                          tags.read(at(down)).r == c.r && tags.read(at(up)).r == c.r;
        if (same) {
            const float z = viewDepth(depths.read(at(p)).r, frame);
            const float zl = viewDepth(depths.read(at(left)).r, frame);
            const float zr = viewDepth(depths.read(at(right)).r, frame);
            const float zd = viewDepth(depths.read(at(down)).r, frame);
            const float zu = viewDepth(depths.read(at(up)).r, frame);
            const float threshold = 10.0 * pixelWorld(z, frame) * float(s);
            const float stepX = abs(zl + zr - 2.0 * z), stepY = abs(zd + zu - 2.0 * z);
            const bool jumpX = stepX > threshold && stepX > 0.8 * 0.5 * abs(zr - zl);
            const bool jumpY = stepY > threshold && stepY > 0.8 * 0.5 * abs(zu - zd);
            if (jumpX || jumpY) {
                solid = max(solid, 0.85);
            }
        }
    }

    // A ghost in front dims the outlines behind it, as it dims everything else.
    if (c.g != 0) {
        solid *= 1.0 - style.ghostAlpha;
    }
    const float ink =
        1.0 - (1.0 - solid * style.inkAlpha) * (1.0 - ghost * style.ghostInk * style.inkAlpha);

    // Print texture on the figure itself: a fine tooth and a slow blotch, kept faint.
    float print = 1.0;
    if (style.grain > 0.0 && (c.r != 0 || c.g != 0 || ink > 0.0)) {
        const float fine = valueNoise(px / (1.35 * style.pixelsPerPoint));
        const float blotch = valueNoise(px / (90.0 * style.pixelsPerPoint));
        print = mix(1.0, (0.955 + 0.07 * fine) * (0.975 + 0.05 * blotch), style.grain);
    }
    return float4(style.ink * ink * print, (1.0 - ink) * print);
}
