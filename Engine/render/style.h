// Render styles, as data. A style decides how surfaces are lit, the sheet behind the model, the
// ink, the shadow and what each kind of material looks like. After the step_viewer styles and the
// Colorfigures palette.
#pragma once

#include "base/types.h"
#include "shaders/shared/gpu_types.h"

#include <array>
#include <optional>
#include <string_view>

namespace geo::render {

enum class StyleName : u32 { Illustration, Metal, Clay, Blueprint, Technical };
inline constexpr usize kStyleCount = 5;

/// What decides a body's colour.
enum class ColorMode : u32 {
    Material, // by what each part is most likely made of, read from its name
    Parts,    // one colour per part
    Mono,     // one material throughout
    File,     // colours stored in the file
};

/// What a part is most likely made of. Each style draws a class its own way.
enum class MaterialClass : u8 {
    Copper,
    Magnet,
    Pcb,
    Epoxy,
    Insulation,
    LaminationWarm,
    LaminationCool,
    Brass,
    Rubber,
    Glass,
    DarkSteel,
    Steel,
    Aluminium,
};

/// How a body should look, before a style decides its colours.
struct Look {
    enum class Kind : u8 { Material, Cycle, Color, Neutral };
    Kind kind = Kind::Neutral;
    MaterialClass material = MaterialClass::Aluminium;
    bool byGroup = false; // a cycle by sub-assembly, rather than by part
    u32 index = 0;        // position in a cycle
    u32 color = 0;        // RGBA8, for a file colour

    bool operator==(const Look&) const = default;
};

/// The material a name suggests, if any. Keywords are matched without case, first match winning.
std::optional<MaterialClass> materialFromName(std::string_view name);

using Rgb = simd_float3; // encoded sRGB, 0 to 1, because colours composite in encoded space

/// The light around a physically lit model, fixed to the camera, in linear radiance.
struct Studio {
    Rgb sky, floor;
    Rgb key;             // a large box light above and to the right
    Rgb band;            // a bright horizon strip, which is what makes metal read as metal
    Rgb fill;            // a weaker box to the left
    float sharpness = 0; // 0 soft-edged boxes, 1 crisp ones
    std::array<float, 2> strips{}; // tall strip lights right and left
    float exposure = 1;
    float contrast = 0;        // 0 a gentle shoulder, 1 a filmic curve
    float metalDiffuse = 0.4f; // the share of diffuse light a metal keeps; polish lowers it
};

struct Style {
    StyleName name;
    std::string_view label;
    std::string_view note;
    bool physical;   // studio light with metal and roughness, rather than three flat bands
    bool colorModes; // whether colour modes change anything
    struct {
        Rgb top, bottom;
        float vignette;
        bool grain;
        Rgb gridColor;
        float gridAlpha;
        float gridPitch; // points, or 0 for no grid
    } sheet;
    bool wobble; // the hand-drawn setting applies
    struct {
        Rgb color;
        float alpha;
        float silhouette, part, crease, highlight; // widths in points
        float fold;  // strength of a body folding in front of itself, 0 to 1
        float ghost; // width of a ghost's dashed lines, points
    } ink;
    float ao;     // ambient occlusion strength
    float capInk; // 0 hatches section caps in the part's own dark tone, 1 in ink
    struct {
        Rgb color;
        float alpha;
        float soft; // blur radius in points
        bool on;    // cast by default
    } shadow;
    float ghostAlpha;
    float ghostLineAlpha;
    Rgb hover;
    float hoverAmount;
    Rgb hideTint;
    std::array<u32, 3> selection; // texels, as for a body
    Studio studio;
};

const Style& styleNamed(StyleName name);

/// A body's three palette texels, RGBA8: light, mid and dark bands for flat lighting, or base
/// colour, finish (metalness and roughness) and section cap colour for studio lighting.
std::array<u32, 3> bodyTexels(const Style& style, const Look& look);

/// One colour that stands for a body under a style, for swatches in the interface: RGBA8, red in
/// the low byte.
u32 swatch(const Style& style, const Look& look);

/// The user's choices that, with a style, decide what a frame looks like.
struct LookSettings {
    StyleName style = StyleName::Illustration;
    ColorMode colorMode = ColorMode::Material;
    u32 ghostStyle = kGhostShell; // a GhostStyle
    bool shadow = true;
    float grain = 1.0f;  // paper tooth and print texture, 0 to 1
    float wobble = 0.0f; // hand-drawn wobble in points, 0 to 7
};

/// The style's uniforms for shaders, at a display scale.
StyleUniforms makeStyleUniforms(const Style& style, const LookSettings& settings,
                                float pixelsPerPoint);

/// Whether a style's composite looks for outlines: silhouettes, part lines or folds.
inline bool drawsOutlines(const StyleUniforms& style) {
    return style.silhouette > 0.0f || style.partLine > 0.0f || style.fold > 0.0f;
}

} // namespace geo::render
