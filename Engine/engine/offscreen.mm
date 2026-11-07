// Headless rendering, for tests and `gcli render`.
#include "engine.h"

#include "base/types.h"
#include "engine/look_state.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "model/face_state.h"
#include "model/visibility.h"
#include "nav/camera.h"
#include "render/overlay.h"
#include "render/view.h"
#include "scene/scene.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <unistd.h>

namespace {

using namespace geo;

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

GeoRenderOptions geo_default_render_options(void) {
    return {1600, 1000, 35.0, 25.0, false, true, geo_default_look(), {}, nullptr};
}

bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, const GeoRenderOptions* options) {
    const u32 width = options->width, height = options->height;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        std::unique_ptr<gpu::Renderer> renderer =
            device ? gpu::Renderer::create(device, shader_library_path) : nullptr;
        if (!renderer) {
            return false;
        }

        const int fd = open(blob_path, O_RDONLY);
        if (fd < 0) {
            std::fprintf(stderr, "Could not open %s.\n", blob_path);
            return false;
        }
        std::vector<std::shared_ptr<scene::MappedSegment>> segments;
        std::string error;
        const bool opened = scene::openCacheFile(fd, segments, error);
        close(fd);
        scene::Scene scene;
        scene::Document& document = scene.document(scene.addDocument());
        bool added = opened;
        for (std::shared_ptr<scene::MappedSegment>& segment : segments) {
            added = added && document.add(std::move(segment), error);
        }
        const std::optional<Aabb> sceneBounds = scene.bounds();
        if (!added || !sceneBounds) {
            std::fprintf(stderr, "%s\n",
                         error.empty() ? "The cache file is empty." : error.c_str());
            return false;
        }
        renderer->syncScene(scene);
        renderer->resize(width, height);
        model::FaceState state;
        state.resize(scene.faceCounts());
        Aabb bounds = *sceneBounds;
        if (options->isolate != nullptr && options->isolate[0] != '\0') {
            if (!isolateByName(scene, options->isolate, state)) {
                std::fprintf(stderr, "No part's name contains %s.\n", options->isolate);
                return false;
            }
            bounds = model::visibleBounds(scene, state).value_or(bounds);
        }
        renderer->syncState(state);
        engine::LookState looks;
        looks.set(engine::lookSettings(options->look));
        looks.apply(*renderer, scene);

        const nav::Viewport viewport{double(width), double(height)};
        const double azimuth = options->azimuth_degrees * M_PI / 180.0;
        const double elevation = options->elevation_degrees * M_PI / 180.0;
        const Vec3 direction =
            simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                              -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
        nav::Camera camera;
        camera.perspective = options->perspective;
        camera.lookAt(bounds.center() + direction, bounds.center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, bounds, viewport, kFitMargin);

        render::ViewState view = render::makeView(camera, viewport, bounds);
        view.uniforms.lineOffset = float(2.0 * scene.coarsestDeflection());
        view.uniforms.aoRadius = float(0.025 * simd_length(bounds.max - bounds.min));
        view.uniforms.aoSamples = 12;
        render::Section section;
        for (int axis = 0; axis < 3; ++axis) {
            section.enabled[axis] = options->section.enabled[axis];
            section.position[axis] = options->section.position[axis];
            section.flip[axis] = options->section.flip[axis];
        }
        render::applySection(section, bounds, view.uniforms);
        render::OverlayLayout layout;
        if (options->look.style == GeoStyleBlueprint) {
            layout.ink = render::kOverlayInkOnDark;
        }
        const render::Overlay overlay =
            render::makeOverlay(camera, viewport, view.uniforms.viewRotation, 2.0f,
                                renderer->glyphAdvance(), 0, layout);
        const std::vector<u8> pixels =
            renderer->renderOffscreen(view, options->overlay ? &overlay : nullptr);
        if (!engine::writePng(png_path, pixels, width, height)) {
            std::fprintf(stderr, "Could not write %s.\n", png_path);
            return false;
        }
    }
    return true;
}
