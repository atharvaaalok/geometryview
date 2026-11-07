#include "engine/look_state.h"

#include "gpu/renderer.h"

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
    }
    settings_ = settings;
    styleDirty_ = true;
}

void LookState::setScale(float pixelsPerPoint) {
    if (pixelsPerPoint != pixelsPerPoint_) {
        pixelsPerPoint_ = pixelsPerPoint;
        styleDirty_ = true;
    }
}

void LookState::apply(gpu::Renderer& renderer, const scene::Scene& scene) {
    const render::Style& style = render::styleNamed(settings_.style);
    if (looksDirty_) {
        looks_ = render::assignLooks(scene, settings_.colorMode);
        looksDirty_ = false;
        paletteDirty_ = true;
    }
    if (paletteDirty_) {
        const std::vector<u32> palette = render::paletteTexels(style, looks_.looks);
        renderer.syncLooks(looks_.ofOccurrence, palette);
        paletteDirty_ = false;
    }
    if (styleDirty_) {
        renderer.setStyle(render::makeStyleUniforms(style, settings_, pixelsPerPoint_),
                          style.physical);
        styleDirty_ = false;
    }
}

} // namespace geo::engine
