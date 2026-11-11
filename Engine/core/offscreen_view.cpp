#include "core/offscreen_view.h"

#include "core/look_state.h"
#include "model/face_state.h"
#include "model/visibility.h"
#include "nav/camera.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <ranges>

namespace geo::engine {
namespace {

constexpr double kFitMargin = 1.12;

bool containsIgnoringCase(std::string_view text, std::string_view part) {
    return std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
               return std::tolower(u8(a)) == std::tolower(u8(b));
           }) != text.end();
}

// Hides every occurrence outside the tree nodes whose names contain one of the comma-separated
// `names`. Returns false if no node matches.
bool isolateByName(const scene::Scene& scene, std::string_view names, model::FaceState& state) {
    std::vector<u8> keep(scene.occurrenceCount(), 0);
    bool any = false;
    const format::SegmentView& model = scene.document(0).model();
    for (const format::NodeRecord& node : model.nodes) {
        const std::string_view name(model.strings.data() + node.nameOffset, node.nameLength);
        for (auto part : std::views::split(names, ',')) {
            const std::string_view filter(part.begin(), part.end());
            if (filter.empty() || !containsIgnoringCase(name, filter)) {
                continue;
            }
            for (u32 o = node.firstOccurrence; o < node.occurrenceEnd && o < keep.size(); ++o) {
                keep[o] = 1;
                any = true;
            }
        }
    }
    for (u32 o = 0; o < keep.size(); ++o) {
        if (keep[o] == 0) {
            state.set({o, model::Target::kWholeOccurrence}, kStateHidden, true);
        }
    }
    return any;
}

} // namespace

bool prepareOffscreen(render::Backend& backend,
                      std::vector<std::shared_ptr<scene::Segment>> segments,
                      const GeoRenderOptions& options, OffscreenView& out, std::string& error) {
    const u32 width = options.width, height = options.height;
    scene::Document& document = out.scene.document(out.scene.addDocument());
    for (std::shared_ptr<scene::Segment>& segment : segments) {
        if (!document.add(std::move(segment), error)) {
            return false;
        }
    }
    const std::optional<Aabb> sceneBounds = out.scene.bounds();
    if (!sceneBounds) {
        error = "The cache file is empty.";
        return false;
    }
    backend.syncScene(out.scene);
    backend.resize(width, height);
    model::FaceState state;
    state.resize(out.scene.faceCounts());
    Aabb bounds = *sceneBounds;
    if (options.isolate != nullptr && options.isolate[0] != '\0') {
        if (!isolateByName(out.scene, options.isolate, state)) {
            error = std::string("No part's name contains ") + options.isolate + ".";
            return false;
        }
        bounds = model::visibleBounds(out.scene, state).value_or(bounds);
    }
    backend.syncState(state);
    const float pixelsPerPoint =
        options.pixels_per_point > 0.0f ? std::clamp(options.pixels_per_point, 0.25f, 8.0f) : 2.0f;
    LookState looks;
    looks.set(lookSettings(options.look));
    looks.setRaster(
        {.pixelsPerPoint = pixelsPerPoint,
         .lineScale = options.line_scale > 0.0f ? std::min(options.line_scale, 4.0f) : 1.0f,
         .sheetAlpha = options.transparent ? 0.0f : 1.0f});
    if (options.first_color >= 0) {
        looks.setFirstColors({u32(options.first_color)});
    }
    looks.apply(backend, out.scene);

    const nav::Viewport viewport{double(width), double(height)};
    const double azimuth = options.azimuth_degrees * std::numbers::pi / 180.0;
    const double elevation = options.elevation_degrees * std::numbers::pi / 180.0;
    const Vec3 direction =
        simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                          -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    nav::Camera camera;
    camera.perspective = options.perspective;
    camera.lookAt(bounds.center() + direction, bounds.center(), simd_make_double3(0.0, 0.0, 1.0));
    nav::fit(camera, bounds, viewport, kFitMargin);
    if (options.world_per_pixel > 0.0 && !options.perspective) {
        camera.orthoHalfHeight = options.world_per_pixel * double(height) * 0.5;
    }
    out.worldPerPixel = 2.0 * camera.halfHeightAt(camera.focusDistance) / double(height);

    out.view = render::makeView(camera, viewport, bounds);
    out.view.uniforms.aoRadius = float(0.025 * simd_length(bounds.max - bounds.min));
    out.view.uniforms.aoSamples = 12;
    render::Section section;
    for (int axis = 0; axis < 3; ++axis) {
        section.enabled[axis] = options.section.enabled[axis];
        section.position[axis] = options.section.position[axis];
        section.flip[axis] = options.section.flip[axis];
    }
    render::applySection(section, bounds, out.view.uniforms);
    render::OverlayLayout layout;
    if (geo_style_on_dark(options.look.style)) {
        layout.ink = render::kOverlayInkOnDark;
    }
    out.overlay = render::makeOverlay(camera, viewport, out.view.uniforms.viewRotation,
                                      pixelsPerPoint, backend.glyphAdvance(), 0, layout);
    return true;
}

} // namespace geo::engine

GeoRenderOptions geo_default_render_options(void) {
    return {1600, 1000,    35.0, 25.0, false, true,  geo_default_look(),
            {},   nullptr, 2.0f, 1.0f, -1,    false, 0.0};
}
