// The look the user chose, and what of it the renderer has yet to hear about.
#pragma once

#include "engine.h"
#include "render/looks.h"
#include "render/style.h"
#include "scene/scene.h"

namespace geo::gpu {
class Renderer;
}

namespace geo::engine {

/// The settings a public look stands for, with out-of-range values brought into range.
render::LookSettings lookSettings(const GeoLook& look);

class LookState {
public:
    void set(const render::LookSettings& settings);
    void setScale(float pixelsPerPoint);
    /// Occurrences were added or removed, so every body's look must be decided again.
    void sceneChanged() { looksDirty_ = true; }

    /// Uploads whatever changed since the last call, which is nothing on most frames.
    void apply(gpu::Renderer& renderer, const scene::Scene& scene);

    const render::LookSettings& settings() const { return settings_; }

private:
    render::LookSettings settings_;
    render::LookAssignment looks_;
    float pixelsPerPoint_ = 2.0f;
    bool looksDirty_ = true;
    bool paletteDirty_ = true;
    bool styleDirty_ = true;
};

} // namespace geo::engine
