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

/// How offscreen renders, the gallery's pictures among them, are drawn. Kept pictures are keyed
/// by it, not by the app's version: bump it when what they draw changes (a style, the framing, the
/// ground, the lines), and only then, so they are drawn again when they look different and kept
/// across updates that don't change them. Tests/unit/golden_test.mm holds it to the thumbnail
/// goldens.
inline constexpr int kPictureDrawing = 1;

struct OffscreenView {
    scene::Scene scene;
    render::ViewState view;
    render::Overlay overlay;
    double worldPerPixel = 0.0; // the scale drawn at, model units per pixel at the focus
};

/// Adds a finished cache file's segments to `out.scene`, brings `backend` up to date with it,
/// its isolation and its look, and frames the view as `options` say. Returns false and sets
/// `error` if the file holds nothing to draw or no part matches the isolation.
bool prepareOffscreen(render::Backend& backend,
                      std::vector<std::shared_ptr<scene::Segment>> segments,
                      const GeoRenderOptions& options, OffscreenView& out, std::string& error);

} // namespace geo::engine
