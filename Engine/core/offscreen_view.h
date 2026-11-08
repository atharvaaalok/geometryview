// What a headless render of a cache file draws, the same for every renderer: the scene, its states
// and looks, the camera fitted to it, and the overlay. `gcli render` and the golden tests use it,
// so Metal and WebGPU draw exactly the same frame.
#pragma once

#include "engine.h"
#include "render/backend.h"
#include "render/overlay.h"
#include "render/view.h"
#include "scene/scene.h"

#include <memory>
#include <string>
#include <vector>

namespace geo::engine {

struct OffscreenView {
    scene::Scene scene;
    render::ViewState view;
    render::Overlay overlay;
};

/// Adds a finished cache file's segments to `out.scene`, brings `backend` up to date with it,
/// its isolation and its look, and frames the view as `options` say. Returns false and sets
/// `error` if the file holds nothing to draw or no part matches the isolation.
bool prepareOffscreen(render::Backend& backend,
                      std::vector<std::shared_ptr<scene::Segment>> segments,
                      const GeoRenderOptions& options, OffscreenView& out, std::string& error);

} // namespace geo::engine
