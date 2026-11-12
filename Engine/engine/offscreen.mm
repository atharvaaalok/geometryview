// Headless rendering: the gallery's thumbnailer, and single pictures for tests and `gcli render`.
#include "engine.h"

#include "base/types.h"
#include "core/offscreen_view.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "scene/segment.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

using namespace geo;

struct GeoThumbnailer {
    std::unique_ptr<gpu::Renderer> renderer;
    std::mutex mutex;
};

namespace {

// The box of pixels with any alpha, or nothing.
bool alphaBounds(const std::vector<u8>& bgra, u32 width, u32 height, uint32_t rect[4]) {
    u32 left = width, top = height, right = 0, bottom = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* row = bgra.data() + usize(y) * width * 4;
        for (u32 x = 0; x < width; ++x) {
            if (row[x * 4 + 3] != 0) {
                left = std::min(left, x);
                right = std::max(right, x + 1);
                top = std::min(top, y);
                bottom = y + 1;
            }
        }
    }
    if (right <= left) {
        return false;
    }
    rect[0] = left;
    rect[1] = top;
    rect[2] = right - left;
    rect[3] = bottom - top;
    return true;
}

// Each n × n square of BGRA pixels averaged into one, rounding to nearest: premultiplied colour
// averages as it should, and so does an opaque picture.
std::vector<u8> averaged(const std::vector<u8>& bgra, u32 width, u32 height, u32 n) {
    const u32 w = width / n, h = height / n, count = n * n;
    std::vector<u8> out(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 sum[4] = {};
            for (u32 dy = 0; dy < n; ++dy) {
                const u8* row = &bgra[(usize(y * n + dy) * width + usize(x) * n) * 4];
                for (u32 dx = 0; dx < n * 4; ++dx) {
                    sum[dx % 4] += row[dx];
                }
            }
            u8* at = &out[(usize(y) * w + x) * 4];
            for (u32 c = 0; c < 4; ++c) {
                at[c] = u8((sum[c] + count / 2) / count);
            }
        }
    }
    return out;
}

bool fail(char* error, u32 capacity, const std::string& message) {
    if (error != nullptr && capacity > 0) {
        std::snprintf(error, capacity, "%s", message.c_str());
    }
    return false;
}

} // namespace

GeoThumbnailer* geo_thumbnailer_create(const char* shader_library_path) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        std::unique_ptr<gpu::Renderer> renderer =
            device ? gpu::Renderer::create(device, shader_library_path) : nullptr;
        if (!renderer) {
            return nullptr;
        }
        auto* thumbnailer = new GeoThumbnailer;
        thumbnailer->renderer = std::move(renderer);
        return thumbnailer;
    }
}

bool geo_thumbnailer_render(GeoThumbnailer* thumbnailer, const char* blob_path,
                            const GeoRenderOptions* options, GeoThumbnail* out, char* error,
                            uint32_t error_capacity) {
    *out = {};
    const std::lock_guard lock(thumbnailer->mutex);
    @autoreleasepool {
        const int fd = open(blob_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return fail(error, error_capacity, std::string("Could not open ") + blob_path + ".");
        }
        std::vector<std::shared_ptr<scene::Segment>> segments;
        std::string message;
        const bool opened = scene::openCacheFile(fd, segments, message);
        close(fd);
        if (!opened) {
            return fail(error, error_capacity, message);
        }
        gpu::Renderer& renderer = *thumbnailer->renderer;
        // Supersampled: drawn n times as large, lines and all, then each n × n square averaged.
        const u32 n = std::clamp(options->supersample, 1u, 4u);
        GeoRenderOptions drawn = *options;
        drawn.width *= n;
        drawn.height *= n;
        drawn.pixels_per_point =
            (options->pixels_per_point > 0.0f ? options->pixels_per_point : 2.0f) * float(n);
        drawn.world_per_pixel = options->world_per_pixel / double(n);
        engine::OffscreenView frame;
        if (!engine::prepareOffscreen(renderer, std::move(segments), drawn, frame, message)) {
            return fail(error, error_capacity, message);
        }
        std::vector<u8> pixels =
            renderer.renderOffscreen(frame.view, options->overlay ? &frame.overlay : nullptr);
        if (n > 1) {
            pixels = averaged(pixels, drawn.width, drawn.height, n);
        }

        out->width = options->width;
        out->height = options->height;
        out->premultiplied = options->transparent;
        out->world_per_pixel = frame.worldPerPixel * double(n);
        if (const std::optional<std::array<u32, 4>> part = renderer.drawnBounds()) {
            // Its pixels at the size kept: every one any of its drawn pixels falls in.
            const u32 x0 = (*part)[0] / n, y0 = (*part)[1] / n;
            const u32 x1 = ((*part)[0] + (*part)[2] + n - 1) / n;
            const u32 y1 = ((*part)[1] + (*part)[3] + n - 1) / n;
            out->part_rect[0] = x0;
            out->part_rect[1] = y0;
            out->part_rect[2] = x1 - x0;
            out->part_rect[3] = y1 - y0;
        }
        if (!options->transparent ||
            !alphaBounds(pixels, options->width, options->height, out->ink_rect)) {
            out->ink_rect[2] = options->transparent ? 0 : options->width;
            out->ink_rect[3] = options->transparent ? 0 : options->height;
        }
        out->bgra = static_cast<uint8_t*>(std::malloc(pixels.size()));
        std::memcpy(out->bgra, pixels.data(), pixels.size());
    }
    return true;
}

void geo_thumbnailer_trim(GeoThumbnailer* thumbnailer) {
    const std::lock_guard lock(thumbnailer->mutex);
    @autoreleasepool {
        thumbnailer->renderer->releaseScene();
    }
}

void geo_thumbnailer_destroy(GeoThumbnailer* thumbnailer) {
    @autoreleasepool {
        delete thumbnailer;
    }
}

bool geo_thumbnail_write_png(const GeoThumbnail* thumbnail, const char* path) {
    if (thumbnail->bgra == nullptr) {
        return false;
    }
    @autoreleasepool {
        const std::vector<u8> pixels(thumbnail->bgra, thumbnail->bgra + usize(thumbnail->width) *
                                                                            thumbnail->height * 4);
        return engine::writePng(path, pixels, thumbnail->width, thumbnail->height,
                                thumbnail->premultiplied ? engine::Alpha::Premultiplied
                                                         : engine::Alpha::Ignore);
    }
}

void geo_thumbnail_free(GeoThumbnail* thumbnail) {
    std::free(thumbnail->bgra);
    *thumbnail = {};
}

bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, const GeoRenderOptions* options) {
    GeoThumbnailer* thumbnailer = geo_thumbnailer_create(shader_library_path);
    if (thumbnailer == nullptr) {
        return false;
    }
    GeoThumbnail picture{};
    char error[512];
    const bool rendered =
        geo_thumbnailer_render(thumbnailer, blob_path, options, &picture, error, sizeof(error));
    geo_thumbnailer_destroy(thumbnailer);
    if (!rendered) {
        std::fprintf(stderr, "%s\n", error);
        return false;
    }
    const bool written = geo_thumbnail_write_png(&picture, png_path);
    geo_thumbnail_free(&picture);
    if (!written) {
        std::fprintf(stderr, "Could not write %s.\n", png_path);
    }
    return written;
}
