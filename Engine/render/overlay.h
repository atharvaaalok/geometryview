// What the overlay draws each frame: the view cube's pose and the scale bar's quads.
#pragma once

#include "base/types.h"
#include "nav/camera.h"
#include "shaders/shared/gpu_types.h"

#include <array>
#include <string_view>

namespace geo::render {

/// Characters the atlas holds for overlay text, in cell order. 'u' stands for the micro sign.
inline constexpr std::string_view kAtlasGlyphs = " 0123456789.kmu";

struct Overlay {
    bool visible = false;
    OverlayUniforms uniforms{};
    std::array<OverlayQuad, kOverlayMaxQuads> quads{};
};

/// Where the overlay sits, in points from the viewport's edges.
struct OverlayLayout {
    float cubeSize = 132; // the square the cube turns in
    float cubeTop = 12;
    float cubeRight = 6;
    float cubeHalfEdge = 30; // half the cube's edge
    float scaleRight = 24;
    float scaleBottom = 96;      // above the toolbar
    float scaleTargetWidth = 96; // the bar is the largest round length up to this
    simd_float4 ink = {0x23 / 255.0f, 0x1E / 255.0f, 0x16 / 255.0f, 1.0f}; // the scale bar's
};

/// The overlay drawn straight on the sheet turns light on Blueprint's dark blue.
inline constexpr simd_float4 kOverlayInkOnDark = {0xEE / 255.0f, 0xF4 / 255.0f, 1.0f, 1.0f};

/// Builds the overlay for a frame. `glyphAdvance` is a glyph's width in atlas pixels, and
/// `pixelsPerPoint` is the display's scale. `cubeHover` is a face index plus one, or zero.
Overlay makeOverlay(const nav::Camera& camera, const nav::Viewport& viewport,
                    const simd_float3x3& viewRotation, float pixelsPerPoint, float glyphAdvance,
                    u32 cubeHover, const OverlayLayout& layout = {});

/// The scale bar's length in world units and its label, such as "20 mm", for a scale in pixels
/// per world unit. Lengths are 1, 2 or 5 times a power of ten, in millimetres.
struct ScaleChoice {
    double length;
    char label[16];
};
ScaleChoice chooseScale(double pixelsPerUnit, double targetPixels);

} // namespace geo::render
