// The look the user chose, and what of it the renderer has yet to hear about.
#pragma once

#include "engine.h"
#include "render/looks.h"
#include "render/style.h"
#include "scene/scene.h"

#include <optional>
#include <vector>

namespace geo::render {
class Backend;
}

namespace geo::engine {

/// The settings a public look stands for, with out-of-range values brought into range.
render::LookSettings lookSettings(const GeoLook& look);

class LookState {
public:
    void set(const render::LookSettings& settings);
    void setScale(float pixelsPerPoint);
    /// Occurrences were added or removed, so every body's look must be decided again, and which
    /// are shell.
    void sceneChanged() {
        looksDirty_ = true;
        shells_.clear();
    }

    /// Where each document's colours start in the cycle, indexed by document; a document without
    /// one counts on from the documents before it.
    void setFirstColors(std::vector<std::optional<u32>> firstColors) {
        if (firstColors != firstColors_) {
            firstColors_ = std::move(firstColors);
            looksDirty_ = true;
        }
    }

    /// Marks which occurrences are the see-through shell, one flag each in global order.
    void setShells(std::vector<u8> shells) {
        shells_ = std::move(shells);
        paletteDirty_ = true;
    }
    bool hasShells() const { return !shells_.empty(); }
    /// Which occurrences are the shell, one flag each in global order; empty until classified.
    const std::vector<u8>& shells() const { return shells_; }

    /// Uploads whatever changed since the last call, which is nothing on most frames.
    void apply(render::Backend& renderer, const scene::Scene& scene);

    const render::LookSettings& settings() const { return settings_; }

    /// Whether the swatch colours changed since they were last taken.
    bool swatchesChanged() const { return swatchesDirty_; }
    /// Each occurrence's swatch colour, RGBA8 in global order, as of the last apply.
    std::vector<u32> takeSwatches();

private:
    render::LookSettings settings_;
    render::LookAssignment looks_;
    std::vector<u8> shells_;
    std::vector<std::optional<u32>> firstColors_;
    float pixelsPerPoint_ = 2.0f;
    bool looksDirty_ = true;
    bool paletteDirty_ = true;
    bool styleDirty_ = true;
    bool swatchesDirty_ = true;
};

} // namespace geo::engine
