#include "render/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace geo::render {
namespace {

constexpr float kBarThickness = 1.5f; // points, the ink line weight
constexpr float kTickHeight = 6.0f;
constexpr float kLabelGap = 4.0f;

} // namespace

ScaleChoice chooseScale(double pixelsPerUnit, double targetPixels) {
    ScaleChoice choice{};
    const double target = targetPixels / std::max(pixelsPerUnit, 1e-30);
    const double power = std::pow(10.0, std::floor(std::log10(target)));
    double length = power;
    for (const double step : {5.0, 2.0, 1.0}) {
        if (step * power <= target) {
            length = step * power;
            break;
        }
    }
    choice.length = length;
    // Lengths are in millimetres. Large and tiny ones read better in metres and micrometres.
    if (length >= 1000.0) {
        std::snprintf(choice.label, sizeof(choice.label), "%g m", length / 1000.0);
    } else if (length < 0.1) {
        std::snprintf(choice.label, sizeof(choice.label), "%g um", length * 1000.0);
    } else {
        std::snprintf(choice.label, sizeof(choice.label), "%g mm", length);
    }
    return choice;
}

Overlay makeOverlay(const nav::Camera& camera, const nav::Viewport& viewport,
                    const simd_float3x3& viewRotation, float pixelsPerPoint, float glyphAdvance,
                    u32 cubeHover, const OverlayLayout& layout) {
    Overlay overlay;
    overlay.visible = true;
    OverlayUniforms& u = overlay.uniforms;
    const float scale = pixelsPerPoint;
    const float width = float(viewport.width), height = float(viewport.height);
    u.viewRotation = viewRotation;
    u.viewport = simd_make_float2(width, height);
    u.cubeCenter = simd_make_float2(width - (layout.cubeRight + layout.cubeSize * 0.5f) * scale,
                                    (layout.cubeTop + layout.cubeSize * 0.5f) * scale);
    u.cubeHalf = layout.cubeHalfEdge * scale;
    u.lineWidth = 1.5f * scale;
    u.cubeHover = cubeHover;

    // The scale bar measures at the target's depth, where perspective and orthographic agree.
    const double pixelsPerUnit =
        viewport.height / (2.0 * camera.halfHeightAt(camera.focusDistance));
    const ScaleChoice choice = chooseScale(pixelsPerUnit, layout.scaleTargetWidth * scale);
    const float barWidth = float(choice.length * pixelsPerUnit);
    const float right = width - layout.scaleRight * scale;
    const float baseline = height - layout.scaleBottom * scale;
    const float thickness = std::max(1.0f, std::round(kBarThickness * scale));
    const float left = std::round(right - barWidth);

    u32 count = 0;
    const auto solid = [&](float x, float y, float w, float h) {
        overlay.quads[count++] = {simd_make_float2(x, y), simd_make_float2(w, h),
                                  simd_make_float4(0, 0, -1, -1), layout.ink};
    };
    solid(left, baseline - thickness, right - left, thickness);
    solid(left, baseline - kTickHeight * scale, thickness, kTickHeight * scale);
    solid(right - thickness, baseline - kTickHeight * scale, thickness, kTickHeight * scale);

    // The label, centred above the bar, one quad per glyph from the atlas strip.
    const usize glyphs = std::strlen(choice.label);
    const float advance = glyphAdvance * (scale / 2.0f); // the atlas is drawn for a 2x display
    const float glyphHeight = float(kAtlasGlyphHeight) * (scale / 2.0f);
    float x = std::round((left + right) * 0.5f - advance * float(glyphs) * 0.5f);
    const float y = std::round(baseline - kTickHeight * scale - kLabelGap * scale - glyphHeight);
    for (usize i = 0; i < glyphs && count < kOverlayMaxQuads; ++i, x += advance) {
        const usize cell = kAtlasGlyphs.find(choice.label[i]);
        if (cell == std::string_view::npos || choice.label[i] == ' ') {
            continue;
        }
        const float cellLeft = float(cell * kAtlasGlyphWidth);
        const float margin = (float(kAtlasGlyphWidth) - glyphAdvance) * 0.5f;
        const float top = float(6 * kAtlasLabelHeight);
        const float atlasWidth = float(kAtlasWidth), atlasHeight = float(kAtlasHeight);
        overlay.quads[count++] = {simd_make_float2(x, y), simd_make_float2(advance, glyphHeight),
                                  simd_make_float4((cellLeft + margin) / atlasWidth,
                                                   (top + float(kAtlasGlyphHeight)) / atlasHeight,
                                                   (cellLeft + margin + glyphAdvance) / atlasWidth,
                                                   top / atlasHeight),
                                  layout.ink};
    }
    u.quadCount = count;
    return overlay;
}

} // namespace geo::render
