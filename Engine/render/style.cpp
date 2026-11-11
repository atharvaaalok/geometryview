#include "render/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace geo::render {
namespace {

constexpr Rgb hex(u32 value) {
    return Rgb{float((value >> 16) & 0xFF) / 255.0f, float((value >> 8) & 0xFF) / 255.0f,
               float(value & 0xFF) / 255.0f};
}

Rgb mix(Rgb a, Rgb b, float t) {
    return a + (b - a) * t;
}

u32 texel(Rgb c, float alpha = 1.0f) {
    const auto byte = [](float v) { return u32(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return byte(c.x) | byte(c.y) << 8 | byte(c.z) << 16 | byte(alpha) << 24;
}

Rgb fromRgba8(u32 color) {
    return Rgb{float(color & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
               float((color >> 16) & 0xFF) / 255.0f};
}

// ---- The figure palette, after Colorfigures by April Zhu (MIT) --------------------------------
// Cobalt is reserved for selection, so it never names a part.

struct Bands {
    Rgb light, mid, dark;
};

constexpr Rgb kInk = hex(0x231E16);
constexpr Rgb kPaper = hex(0xF2EFE7);
constexpr Rgb kCream = hex(0xFFF4D6);
constexpr Rgb kShadowBlue = hex(0x41538F);
constexpr Rgb kWhite = hex(0xFFFFFF);

// Light and mid are Colorfigures' own. The darks keep each hue rather than sinking toward olive
// and maroon, because on smooth CAD surfaces a third of every part sits in the dark band.
constexpr Bands kMustard = {hex(0xF4B829), hex(0xD49C1F), hex(0x9A6D14)};
constexpr Bands kTomato = {hex(0xEF6A4C), hex(0xCF5038), hex(0x9C3A26)};
constexpr Bands kMint = {hex(0x93D3B0), hex(0x71B693), hex(0x3F7A5E)};
constexpr Bands kBlush = {hex(0xF3A6BB), hex(0xD8869C), hex(0xA65A6E)};

// Metal is a warm aluminium so it sits on the paper. Dark materials stay far enough from the ink
// that outlines still read on them.
constexpr Bands kMetal = {hex(0xF3F1EC), hex(0xD6D2C8), hex(0xA8A398)};
constexpr Bands kClear = {hex(0xE4F2F4), hex(0xC4DFE4), hex(0x97B9C0)};
constexpr Bands kCeramic = {hex(0xFBFAF3), hex(0xE3E0D2), hex(0xBDB8A6)};
constexpr Bands kDark = {hex(0x8A847B), hex(0x6E6961), hex(0x56524B)};
constexpr Bands kGraphiteCool = {hex(0x9A9EA3), hex(0x7C8187), hex(0x5F646A)};
constexpr Bands kGraphiteWarm = {hex(0xA39A8E), hex(0x857C71), hex(0x665E55)};
constexpr Bands kWood = {hex(0xD9A86C), hex(0xBF8A4F), hex(0x8E6236)};

// A light cobalt ramp, never as dark as the cast shadow it may sit on.
constexpr Bands kSelection = {hex(0xCDDCF6), hex(0xA6C1EF), hex(0x7A9FDC)};

// Swatches that tell parts apart, in order.
constexpr std::array<Bands, 6> kCycle = {kMustard, kMint, kTomato, kBlush, kWood, kClear};

Bands illustratedBands(MaterialClass material) {
    switch (material) {
    case MaterialClass::Copper:
        return kMustard;
    case MaterialClass::Magnet:
        return kTomato;
    case MaterialClass::Pcb:
        return kMint;
    case MaterialClass::Epoxy:
    case MaterialClass::Insulation:
        return kCeramic;
    case MaterialClass::LaminationWarm:
        return kGraphiteWarm;
    case MaterialClass::LaminationCool:
        return kGraphiteCool;
    case MaterialClass::Brass:
        return kWood;
    case MaterialClass::Glass:
        return kClear;
    case MaterialClass::Rubber:
    case MaterialClass::DarkSteel:
        return kDark;
    case MaterialClass::Steel:
    case MaterialClass::Aluminium:
        return kMetal;
    }
    return kMetal;
}

// Bands for any colour: mid 16% and dark 42% toward black, as Colorfigures derives them.
Bands bandsOf(Rgb light) {
    return {light, mix(light, Rgb{0, 0, 0}, 0.16f), mix(light, Rgb{0, 0, 0}, 0.42f)};
}

Bands illustrated(const Look& look) {
    switch (look.kind) {
    case Look::Kind::Material:
        return illustratedBands(look.material);
    case Look::Kind::Cycle:
        return kCycle[look.index % kCycle.size()];
    case Look::Kind::Color:
        return bandsOf(fromRgba8(look.color));
    case Look::Kind::Neutral:
        return kCeramic;
    }
    return kCeramic;
}

// ---- Studio finishes -------------------------------------------------------------------------

struct Finish {
    Rgb base;
    float metal; // 0 dielectric, 1 metal
    float rough; // 0 mirror, 1 matte
};

// Satin finishes, as machined parts look under a studio light, each material in a clear colour of
// its own so a drive reads at a glance.
Finish metalOf(MaterialClass material) {
    switch (material) {
    case MaterialClass::Steel:
        return {hex(0xB3B7BB), 1.0f, 0.48f};
    case MaterialClass::Aluminium:
        return {hex(0xC6CACD), 1.0f, 0.55f};
    case MaterialClass::DarkSteel:
        return {hex(0x80858B), 1.0f, 0.48f};
    case MaterialClass::Copper:
        return {hex(0xE39252), 1.0f, 0.38f};
    case MaterialClass::Brass:
        return {hex(0xD9B162), 1.0f, 0.42f};
    case MaterialClass::Magnet:
        return {hex(0xD0493A), 0.25f, 0.4f};
    case MaterialClass::LaminationCool:
        return {hex(0x66738A), 1.0f, 0.52f};
    case MaterialClass::LaminationWarm:
        return {hex(0x74787D), 1.0f, 0.52f};
    case MaterialClass::Epoxy:
        return {hex(0x303338), 0.0f, 0.38f};
    case MaterialClass::Insulation:
        return {hex(0xEAD9AB), 0.0f, 0.62f};
    case MaterialClass::Pcb:
        return {hex(0x2D9463), 0.0f, 0.3f};
    case MaterialClass::Rubber:
        return {hex(0x2B2B2E), 0.0f, 0.72f};
    case MaterialClass::Glass:
        return {hex(0xC5DDE3), 0.0f, 0.08f};
    }
    return {hex(0xC6CACD), 1.0f, 0.55f};
}

Finish metalFinish(const Look& look) {
    switch (look.kind) {
    case Look::Kind::Material:
        return metalOf(look.material);
    case Look::Kind::Cycle:
        if (look.byGroup) {
            // Plain metals, alternating, for groups whose names tell nothing.
            const std::array<Finish, 4> metals = {
                metalOf(MaterialClass::Aluminium), metalOf(MaterialClass::Steel),
                Finish{hex(0x7F8B99), 0.25f, 0.5f}, metalOf(MaterialClass::DarkSteel)};
            return metals[look.index % metals.size()];
        }
        return {kCycle[look.index % kCycle.size()].light, 0.25f, 0.4f};
    case Look::Kind::Color:
        return {fromRgba8(look.color), 0.15f, 0.4f}; // a painted part
    case Look::Kind::Neutral:
        return metalOf(MaterialClass::Aluminium);
    }
    return metalOf(MaterialClass::Aluminium);
}

// Polished: a metal's roughness halved, as a part fresh from the lathe and lapped, though never
// mirror-smooth, which no studio box could light without aliasing. Paints and plastics are as in
// Metal.
Finish studioFinish(const Look& look) {
    if (look.kind == Look::Kind::Neutral) {
        return {hex(0xC8CBCE), 1.0f, 0.22f}; // stainless
    }
    Finish f = metalFinish(look);
    if (f.metal >= 0.5f) {
        f.rough = std::max(0.15f, f.rough * 0.5f);
    }
    return f;
}

constexpr Rgb kClay = hex(0xCFC5B7);

Finish clayFinish(const Look& look) {
    const auto tint = [](Rgb color, float t) { return Finish{mix(kClay, color, t), 0.0f, 0.85f}; };
    switch (look.kind) {
    case Look::Kind::Material:
        switch (look.material) {
        case MaterialClass::Copper:
            return tint(kMustard.light, 0.3f);
        case MaterialClass::Magnet:
            return tint(kTomato.light, 0.26f);
        case MaterialClass::Pcb:
            return tint(kMint.mid, 0.3f);
        case MaterialClass::LaminationCool:
            return tint(hex(0x868A90), 0.3f);
        case MaterialClass::LaminationWarm:
            return tint(hex(0x8D8276), 0.3f);
        case MaterialClass::Rubber:
        case MaterialClass::DarkSteel:
            return tint(hex(0x6A645E), 0.4f);
        default:
            return tint(kClay, 0.0f);
        }
    case Look::Kind::Cycle:
        return tint(kCycle[look.index % kCycle.size()].light, 0.36f);
    case Look::Kind::Color:
        return tint(fromRgba8(look.color), 0.45f);
    case Look::Kind::Neutral:
        return tint(kClay, 0.0f);
    }
    return tint(kClay, 0.0f);
}

std::array<u32, 3> bandTexels(const Bands& b) {
    return {texel(b.light), texel(b.mid), texel(b.dark)};
}

std::array<u32, 3> finishTexels(const Finish& f, float capLift) {
    const u32 finish =
        u32(std::lround(f.metal * 255.0f)) | u32(std::lround(f.rough * 255.0f)) << 8 | 0xFFu << 24;
    return {texel(f.base), finish, texel(mix(f.base, kWhite, capLift))};
}

// Faces barely lighter than the blueprint's sheet, in two close tones: the drawing is in the lines.
constexpr Rgb kBlueprintSheet = hex(0x1C4076);
const Bands kBlueprintFace = {mix(kBlueprintSheet, kWhite, 0.14f),
                              mix(kBlueprintSheet, kWhite, 0.11f),
                              mix(kBlueprintSheet, kWhite, 0.07f)};
// White faces: only the lines draw the form, as on a patent drawing.
constexpr Bands kTechnicalFace = {kWhite, kWhite, kWhite};

std::array<Style, kStyleCount> makeStyles() {
    const Rgb none{0, 0, 0};
    Style illustration{
        .name = StyleName::Illustration,
        .label = "Illustration",
        .note = "flat light, ink and paper",
        .physical = false,
        .colorModes = true,
        .dark = false,
        .sheet = {kPaper, kPaper, 0.0f, true, none, 0.0f, 0.0f},
        .wobble = true,
        .ink = {kInk, 1.0f, 2.4f, 1.5f, 1.0f, 2.0f, 0.85f, 1.0f},
        .ao = 0.0f,
        .capInk = 0.0f,
        .shadow = {kShadowBlue, 0.9f, 1.0f, true},
        .ghostAlpha = 0.24f,
        .ghostLineAlpha = 0.5f,
        .hover = kCream,
        .hoverAmount = 0.45f,
        .hideTint = kTomato.light,
        .selection = bandTexels(kSelection),
        .studio = {},
    };
    Style metal{
        .name = StyleName::Metal,
        .label = "Metal",
        .note = "studio light on machined parts",
        .physical = true,
        .colorModes = true,
        .dark = false,
        .sheet = {hex(0xF6F5F3), hex(0xF6F5F3), 0.0f, false, none, 0.0f, 0.0f},
        .wobble = false,
        .ink = {hex(0x45433F), 0.9f, 1.25f, 1.0f, 0.8f, 2.0f, 0.85f, 0.8f},
        .ao = 0.85f,
        .capInk = 0.0f,
        .shadow = {hex(0x2A2F35), 0.16f, 10.0f, true},
        .ghostAlpha = 0.2f,
        .ghostLineAlpha = 0.45f,
        .hover = kWhite,
        .hoverAmount = 0.28f,
        .hideTint = kTomato.light,
        .selection = finishTexels({hex(0x7EA2E0), 0.0f, 0.42f}, 0.18f),
        .studio = {Rgb{0.62f, 0.63f, 0.64f},
                   Rgb{0.2f, 0.2f, 0.21f},
                   Rgb{1.12f, 1.1f, 1.05f},
                   Rgb{0.38f, 0.38f, 0.38f},
                   Rgb{0.42f, 0.44f, 0.47f},
                   0.2f,
                   {0.45f, 0.25f},
                   1.1f,
                   0.7f},
    };
    Style clay{
        .name = StyleName::Clay,
        .label = "Clay",
        .note = "one matte material, soft light",
        .physical = true,
        .colorModes = true,
        .dark = false,
        .sheet = {hex(0xF5F3EF), hex(0xE9E5DE), 0.06f, false, none, 0.0f, 0.0f},
        .wobble = false,
        .ink = {hex(0x3D362D), 0.55f, 1.5f, 0.9f, 0.7f, 2.0f, 0.85f, 0.7f},
        .ao = 0.9f,
        .capInk = 0.0f,
        .shadow = {hex(0x3A332A), 0.2f, 12.0f, true},
        .ghostAlpha = 0.22f,
        .ghostLineAlpha = 0.45f,
        .hover = kWhite,
        .hoverAmount = 0.3f,
        .hideTint = kTomato.light,
        .selection = finishTexels({hex(0x8EACDE), 0.0f, 0.8f}, 0.22f),
        .studio = {Rgb{1.0f, 0.98f, 0.95f},
                   Rgb{0.7f, 0.67f, 0.62f},
                   Rgb{0.95f, 0.92f, 0.86f},
                   Rgb{0, 0, 0},
                   Rgb{0.3f, 0.31f, 0.33f},
                   0.0f,
                   {0.0f, 0.0f},
                   1.0f,
                   0.3f},
    };
    Style blueprint{
        .name = StyleName::Blueprint,
        .label = "Blueprint",
        .note = "white lines on a blue sheet",
        .physical = false,
        .colorModes = false,
        .dark = true,
        .sheet = {hex(0x1E4479), hex(0x173765), 0.18f, true, kWhite, 0.06f, 24.0f},
        .wobble = false,
        .ink = {hex(0xEEF4FF), 0.95f, 1.8f, 1.2f, 0.9f, 2.2f, 0.85f, 0.9f},
        .ao = 0.0f,
        .capInk = 1.0f,
        .shadow = {hex(0x0D2244), 0.35f, 8.0f, false},
        .ghostAlpha = 0.18f,
        .ghostLineAlpha = 0.55f,
        .hover = hex(0x9CC3FF),
        .hoverAmount = 0.35f,
        .hideTint = hex(0xFF9D7A),
        .selection = bandTexels({hex(0xFFD98A), hex(0xF0C46A), hex(0xD7A74A)}),
        .studio = {},
    };
    Style technical{
        .name = StyleName::Technical,
        .label = "Technical",
        .note = "black linework on white",
        .physical = false,
        .colorModes = false,
        .dark = false,
        .sheet = {kWhite, kWhite, 0.0f, false, none, 0.0f, 0.0f},
        .wobble = false,
        .ink = {hex(0x15171A), 1.0f, 2.0f, 1.2f, 1.0f, 2.0f, 0.85f, 1.0f},
        .ao = 0.0f,
        .capInk = 1.0f,
        .shadow = {hex(0x15171A), 0.12f, 6.0f, false},
        .ghostAlpha = 0.2f,
        .ghostLineAlpha = 0.55f,
        .hover = kCream,
        .hoverAmount = 0.6f,
        .hideTint = kTomato.light,
        .selection = bandTexels(kSelection),
        .studio = {},
    };
    Style studio{
        .name = StyleName::Studio,
        .label = "Studio",
        .note = "polished metal under studio lights",
        .physical = true,
        .colorModes = true,
        .dark = true,
        // A charcoal sweep, darker above, as a backdrop seen from a little above the floor.
        .sheet = {hex(0x17181B), hex(0x3A3C40), 0.3f, false, none, 0.0f, 0.0f},
        .wobble = false,
        // No lines on the model: only a highlight, in a light ink that reads on dark metal.
        .ink = {hex(0xE8EEF5), 0.9f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.8f},
        .ao = 0.9f,
        .capInk = 0.0f,
        .shadow = {hex(0x000000), 0.5f, 14.0f, true},
        .ghostAlpha = 0.3f,
        .ghostLineAlpha = 0.55f,
        .hover = kWhite,
        .hoverAmount = 0.18f,
        .hideTint = kTomato.light,
        .selection = finishTexels({hex(0x7EA2E0), 0.5f, 0.3f}, 0.25f),
        // A dark room, so that flat faces turned to the camera mirror darkness and curved ones
        // sweep through the lights: a soft overhead sky for the tops, a strong key box, a bright
        // horizon band and faint strips. At a three-quarter view a flat face mirrors a strip
        // exactly, so the strips stay low.
        .studio = {Rgb{0.55f, 0.555f, 0.56f},
                   Rgb{0.08f, 0.08f, 0.085f},
                   Rgb{1.8f, 1.76f, 1.7f},
                   Rgb{0.75f, 0.75f, 0.75f},
                   Rgb{0.06f, 0.062f, 0.065f},
                   0.45f,
                   {0.5f, 0.3f},
                   1.0f,
                   0.9f,
                   0.06f},
    };
    return {illustration, metal, clay, blueprint, technical, studio};
}

// ---- Material names --------------------------------------------------------------------------

bool has(std::string_view text, std::string_view word) {
    return text.find(word) != std::string_view::npos;
}

bool hasAny(std::string_view text, std::initializer_list<std::string_view> words) {
    return std::any_of(words.begin(), words.end(),
                       [&](std::string_view w) { return has(text, w); });
}

// `first` somewhere before any of `then`.
bool hasThen(std::string_view text, std::string_view first,
             std::initializer_list<std::string_view> then) {
    const usize at = text.find(first);
    return at != std::string_view::npos && hasAny(text.substr(at + first.size()), then);
}

bool isWordCharacter(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// "_core" followed by the end or by something that is not part of a word.
bool hasCoreSuffix(std::string_view text) {
    if (text.ends_with("core")) {
        return true;
    }
    for (usize at = text.find("_core"); at != std::string_view::npos;
         at = text.find("_core", at + 1)) {
        const usize end = at + 5;
        if (end == text.size() || !isWordCharacter(text[end])) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<MaterialClass> materialFromName(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const std::string_view n = lower;
    if (hasAny(n, {"coil", "winding", "wire", "copper"})) {
        return MaterialClass::Copper;
    }
    if (has(n, "magnet")) {
        return MaterialClass::Magnet;
    }
    if (hasAny(n, {"pcb", "board", "circuit", "hall", "sensor"})) {
        return MaterialClass::Pcb;
    }
    if (hasAny(n, {"potting", "epoxy", "resin", "encapsul"})) {
        return MaterialClass::Epoxy;
    }
    if (hasAny(n, {"liner", "insulat", "nomex", "paper"})) {
        return MaterialClass::Insulation;
    }
    if (hasThen(n, "rotor", {"core", "iron", "lamination"}) ||
        hasAny(n, {"backiron", "back_iron", "yoke"})) {
        return MaterialClass::LaminationWarm;
    }
    if (hasThen(n, "stator", {"core", "lamination"}) || has(n, "lamination") || hasCoreSuffix(n)) {
        return MaterialClass::LaminationCool;
    }
    if (hasAny(n, {"cage", "retainer", "separator"})) {
        return MaterialClass::Brass;
    }
    if (hasAny(n, {"cable", "lead", "grommet"})) {
        return MaterialClass::Rubber;
    }
    if (hasAny(n, {"glass", "window", "lens", "cover_clear"})) {
        return MaterialClass::Glass;
    }
    if (has(n, "spring")) {
        return MaterialClass::DarkSteel;
    }
    if (hasAny(n, {"shaft", "ring", "ball", "roller", "screw", "bolt", "nut", "washer"})) {
        return MaterialClass::Steel;
    }
    if (hasAny(n, {"housing", "flange", "cap", "cover", "bell", "hub", "plate", "bracket", "frame",
                   "case", "body", "base"})) {
        return MaterialClass::Aluminium;
    }
    return std::nullopt;
}

const Style& styleNamed(StyleName name) {
    static const std::array<Style, kStyleCount> styles = makeStyles();
    return styles[std::min<usize>(usize(name), kStyleCount - 1)];
}

std::array<u32, 3> bodyTexels(const Style& style, const Look& look) {
    switch (style.name) {
    case StyleName::Illustration:
        return bandTexels(illustrated(look));
    case StyleName::Metal:
        return finishTexels(metalFinish(look), 0.18f);
    case StyleName::Clay:
        return finishTexels(clayFinish(look), 0.22f);
    case StyleName::Blueprint:
        return bandTexels(kBlueprintFace);
    case StyleName::Technical:
        return bandTexels(kTechnicalFace);
    case StyleName::Studio:
        return finishTexels(studioFinish(look), 0.25f);
    }
    return bandTexels(kCeramic);
}

u32 swatch(const Style& style, const Look& look) {
    switch (style.name) {
    case StyleName::Illustration:
        return texel(illustrated(look).light);
    case StyleName::Metal:
        return texel(metalFinish(look).base);
    case StyleName::Clay:
        return texel(clayFinish(look).base);
    case StyleName::Blueprint:
        return texel(kBlueprintFace.light);
    case StyleName::Technical:
        return texel(kWhite);
    case StyleName::Studio:
        return texel(studioFinish(look).base);
    }
    return texel(kCeramic.light);
}

StyleUniforms makeStyleUniforms(const Style& style, const LookSettings& settings,
                                const Raster& raster) {
    const float px = raster.pixelsPerPoint;
    // Ink widths are in pixels of the line's weight; grain, grid and sampling steps are not.
    const float line = px * std::max(raster.lineScale, 0.0f);
    StyleUniforms u{};
    u.sheetTop = style.sheet.top;
    u.sheetBottom = style.sheet.bottom;
    u.gridColor = style.sheet.gridColor;
    // Grain darkens toward a warm grey on paper, and toward a deeper sheet colour elsewhere.
    u.grainTint = style.name == StyleName::Illustration
                      ? Rgb{0.42f, 0.39f, 0.33f}
                      : mix(style.sheet.bottom, Rgb{0, 0, 0}, 0.55f);
    u.ink = style.ink.color;
    u.hover = style.hover;
    u.hideTint = style.hideTint;
    u.shadowColor = style.shadow.color;
    u.lightExterior = simd_normalize(simd_make_float3(0.561f, 0.809f, 0.175f));
    u.lightInterior = simd_normalize(simd_make_float3(0.6f, 0.2f, 1.0f));
    u.sky = style.studio.sky;
    u.floor = style.studio.floor;
    u.key = style.studio.key;
    u.band = style.studio.band;
    u.fill = style.studio.fill;
    u.selection = simd_make_uint4(style.selection[0], style.selection[1], style.selection[2], 0);
    u.strips = simd_make_float2(style.studio.strips[0], style.studio.strips[1]);
    u.vignette = style.sheet.vignette;
    u.gridAlpha = style.sheet.gridAlpha;
    u.gridPitch = style.sheet.gridPitch * px;
    u.grain = style.sheet.grain ? std::clamp(settings.grain, 0.0f, 1.0f) : 0.0f;
    u.inkAlpha = style.ink.alpha;
    u.silhouette = style.ink.silhouette * line;
    u.partLine = style.ink.part * line;
    u.crease = style.ink.crease * line;
    u.highlight = style.ink.highlight * line;
    u.ghostInk = 0.4f;
    u.ghostAlpha = style.ghostAlpha;
    u.ghostLineAlpha = style.ghostLineAlpha;
    u.hoverAmount = style.hoverAmount;
    u.ao = style.ao;
    u.shadowAlpha = style.shadow.alpha;
    u.shadowSoft = style.shadow.soft <= 1.0f ? 1.0f : std::max(style.shadow.soft * line, 1.5f);
    u.wobble = style.wobble ? std::clamp(settings.wobble, 0.0f, 7.0f) * line * 0.7f : 0.0f;
    u.wobblePeriod = 36.0f * px;
    u.pixelsPerPoint = px;
    u.sharpness = style.studio.sharpness;
    u.exposure = style.studio.exposure;
    u.contrast = style.studio.contrast;
    u.ghostStyle = settings.ghostStyle;
    u.shadowOn = settings.shadow ? 1u : 0u;
    u.capInk = style.capInk;
    u.metalDiffuse = style.studio.metalDiffuse;
    u.fold = style.ink.fold;
    u.ghostLine = style.ink.ghost * line;
    return u;
}

} // namespace geo::render
