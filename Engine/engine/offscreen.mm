// Headless rendering, for tests and `gcli render`.
#include "engine.h"

#include "base/types.h"
#include "core/offscreen_view.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "scene/segment.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using namespace geo;

bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, const GeoRenderOptions* options) {
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
        std::vector<std::shared_ptr<scene::Segment>> segments;
        std::string error;
        const bool opened = scene::openCacheFile(fd, segments, error);
        close(fd);
        engine::OffscreenView frame;
        if (!opened ||
            !engine::prepareOffscreen(*renderer, std::move(segments), *options, frame, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        const std::vector<u8> pixels =
            renderer->renderOffscreen(frame.view, options->overlay ? &frame.overlay : nullptr);
        const engine::Alpha alpha =
            options->transparent ? engine::Alpha::Premultiplied : engine::Alpha::Ignore;
        if (!engine::writePng(png_path, pixels, options->width, options->height, alpha)) {
            std::fprintf(stderr, "Could not write %s.\n", png_path);
            return false;
        }
    }
    return true;
}
