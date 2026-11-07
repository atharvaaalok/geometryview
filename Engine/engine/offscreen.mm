// Headless rendering, for tests and `gcli render`.
#include "engine.h"

#include "base/types.h"
#include "engine/look_state.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "nav/camera.h"
#include "render/overlay.h"
#include "render/view.h"
#include "scene/scene.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

using namespace geo;

constexpr double kFitMargin = 1.12;

} // namespace

GeoRenderOptions geo_default_render_options(void) {
    return {1600, 1000, 35.0, 25.0, false, true, geo_default_look(), {}};
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
        engine::LookState looks;
        looks.set(engine::lookSettings(options->look));
        looks.apply(*renderer, scene);

        const nav::Viewport viewport{double(width), double(height)};
        const Aabb bounds = *sceneBounds;
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
