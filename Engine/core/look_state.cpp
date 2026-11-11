#include "core/look_state.h"

#include "render/backend.h"

#include <algorithm>

namespace geo::engine {

render::LookSettings lookSettings(const GeoLook& look) {
    render::LookSettings s;
    s.style = render::StyleName(std::min<u32>(u32(look.style), render::kStyleCount - 1));
    s.colorMode = render::ColorMode(std::min<u32>(u32(look.color_mode), u32(GeoColorFile)));
    s.ghostStyle = std::clamp<u32>(u32(look.ghost_style), kGhostShell, kGhostBoth);
    s.shadow = look.cast_shadow;
    s.grain = std::clamp(look.grain, 0.0f, 1.0f);
    s.wobble = std::clamp(look.wobble, 0.0f, 7.0f);
    return s;
}

void LookState::set(const render::LookSettings& settings) {
    if (settings.colorMode != settings_.colorMode) {
        looksDirty_ = true;
    }
    if (settings.style != settings_.style) {
        paletteDirty_ = true;
        swatchesDirty_ = true;
    }
    settings_ = settings;
    styleDirty_ = true;
}

void LookState::setRaster(const render::Raster& raster) {
    if (raster != raster_) {
        raster_ = raster;
        styleDirty_ = true;
    }
}

void LookState::apply(render::Backend& renderer, const scene::Scene& scene) {
    const render::Style& style = render::styleNamed(settings_.style);
    if (looksDirty_) {
        looks_ = render::assignLooks(scene, settings_.colorMode, firstColors_);
        looksDirty_ = false;
        paletteDirty_ = true;
        swatchesDirty_ = true;
    }
    if (paletteDirty_) {
        const std::vector<u32> palette = render::paletteTexels(style, looks_.looks);
        if (shells_.size() == looks_.ofOccurrence.size()) {
            std::vector<u16> marked = looks_.ofOccurrence;
            for (usize o = 0; o < marked.size(); ++o) {
                marked[o] = u16(marked[o] | (shells_[o] != 0 ? kLookShell : 0));
            }
            renderer.syncLooks(marked, palette);
        } else {
            renderer.syncLooks(looks_.ofOccurrence, palette);
        }
        paletteDirty_ = false;
    }
    if (styleDirty_) {
        renderer.setStyle(render::makeStyleUniforms(style, settings_, raster_), style.physical);
        styleDirty_ = false;
    }
}

std::vector<u32> LookState::takeSwatches() {
    swatchesDirty_ = false;
    return render::swatchColors(render::styleNamed(settings_.style), looks_);
}

} // namespace geo::engine

// The public defaults, here so every platform shares them.
GeoLook geo_default_look(void) {
    return {GeoStyleIllustration, GeoColorParts, GeoGhostShell, true, 1.0f, 0.0f};
}

namespace {
const geo::render::Style& styleOf(GeoStyle style) {
    return geo::render::styleNamed(geo::render::StyleName(
        std::min<geo::u32>(geo::u32(style), geo::render::kStyleCount - 1)));
}
} // namespace

bool geo_style_casts_shadow(GeoStyle style) {
    return styleOf(style).shadow.on;
}

bool geo_style_on_dark(GeoStyle style) {
    return styleOf(style).dark;
}

GeoSheet geo_style_sheet(GeoStyle style) {
    const geo::render::Style& s = styleOf(style);
    // The grain's tint is derived where the shaders get it.
    const StyleUniforms u = geo::render::makeStyleUniforms(s, {}, 1.0f);
    const auto copy = [](geo::render::Rgb c, float* out) {
        out[0] = c.x;
        out[1] = c.y;
        out[2] = c.z;
    };
    GeoSheet sheet{};
    copy(s.sheet.top, sheet.top);
    copy(s.sheet.bottom, sheet.bottom);
    sheet.vignette = s.sheet.vignette;
    copy(s.sheet.gridColor, sheet.grid);
    sheet.grid_alpha = s.sheet.gridAlpha;
    sheet.grid_pitch = s.sheet.gridPitch;
    copy(u.grainTint, sheet.grain_tint);
    sheet.grain = s.sheet.grain;
    copy(s.ink.color, sheet.ink);
    sheet.on_dark = s.dark;
    return sheet;
}
