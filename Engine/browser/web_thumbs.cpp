// The gallery's pictures in the browser: the engine's WebGPU renderer in a worker of its own, with
// a device of its own, drawing cache files one at a time as the Mac's thumbnailer does. A page's
// thumbnail worker (Web/viewer/thumbs-worker.js) loads it and talks to it through the thumbs_*
// functions below; it answers through `Module.thumbsHost`:
//
//   thumbsHost.ready() / thumbsHost.failed(message)    once the GPU is set up, or cannot be
//   thumbsHost.drawn(pixels, width, height, worldPerPixel, rects, message)
//        a picture: straight-alpha RGBA, the scale it was drawn at, and the part's box and every
//        drawn pixel's box (x, y, width, height each); or no pixels and why
//
// One renderer is kept for every picture, and holds only the last file drawn. WebAssembly memory
// never shrinks, so the worker that loads this module is replaced once its heap grows past a
// limit, as the import worker's OCCT workers are.
#include "core/offscreen_view.h"
#include "scene/segment.h"
#include "webgpu/renderer.h"

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace geo;

namespace {

// JavaScript, which clang-format would break.
// clang-format off
EM_JS(void, jsReady, (), { Module.thumbsHost.ready(); });
EM_JS(void, jsFailed, (const char* message), { Module.thumbsHost.failed(UTF8ToString(message)); });
EM_JS(void, jsDrawn, (const u8* pixels, u32 width, u32 height, double worldPerPixel,
                      const u32* rects, const char* message), {
    Module.thumbsHost.drawn(pixels ? HEAPU8.slice(pixels, pixels + width * height * 4) : null,
                            width, height, worldPerPixel,
                            Array.from(HEAPU32.subarray(rects >> 2, (rects >> 2) + 8)),
                            UTF8ToString(message));
});
// clang-format on

struct Thumbs {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    std::unique_ptr<webgpu::Renderer> renderer;
    std::unique_ptr<engine::OffscreenView> frame; // the picture being drawn, then the last drawn
    std::vector<u8> staging;
    bool busy = false;
};

Thumbs thumbs;

void drawnNothing(const std::string& message) {
    thumbs.busy = false;
    const std::array<u32, 8> rects{};
    jsDrawn(nullptr, 0, 0, 0.0, rects.data(), message.c_str());
}

// The box of the pixels that pass `drawn`, as x, y, width and height; zero if none.
template <typename Drawn>
std::array<u32, 4> boundsOf(u32 width, u32 height, Drawn drawn) {
    u32 left = width, top = height, right = 0, bottom = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (drawn(x, y)) {
                left = std::min(left, x);
                right = std::max(right, x + 1);
                top = std::min(top, y);
                bottom = y + 1;
            }
        }
    }
    if (right <= left) {
        return {};
    }
    return {left, top, right - left, bottom - top};
}

void deviceReady() {
    std::string error;
    thumbs.renderer = webgpu::Renderer::create(thumbs.instance, thumbs.device,
                                               wgpu::TextureFormat::BGRA8Unorm, error);
    if (!thumbs.renderer) {
        jsFailed(error.c_str());
        return;
    }
    jsReady();
}

} // namespace

extern "C" {

/// Asks for the GPU's adapter and a device of this worker's own. Answers through
/// thumbsHost.ready or .failed.
EMSCRIPTEN_KEEPALIVE void thumbs_start() {
    thumbs.instance = wgpu::CreateInstance(nullptr);
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    thumbs.instance.RequestAdapter(
        &options, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                jsFailed(std::string(message.data, message.length).c_str());
                return;
            }
            thumbs.adapter = std::move(adapter);
            wgpu::Limits adapterLimits{};
            thumbs.adapter.GetLimits(&adapterLimits);
            static wgpu::Limits limits{};
            limits.maxBufferSize = adapterLimits.maxBufferSize;
            limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
            static wgpu::DeviceDescriptor descriptor{};
            descriptor.requiredLimits = &limits;
            descriptor.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView error) {
                    std::fprintf(stderr, "webgpu: %.*s\n", int(error.length), error.data);
                });
            thumbs.adapter.RequestDevice(
                &descriptor, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus deviceStatus, wgpu::Device device,
                   wgpu::StringView deviceMessage) {
                    if (deviceStatus != wgpu::RequestDeviceStatus::Success) {
                        jsFailed(std::string(deviceMessage.data, deviceMessage.length).c_str());
                        return;
                    }
                    thumbs.device = std::move(device);
                    deviceReady();
                });
        });
}

/// Room for a cache file of `size` bytes, to be filled before thumbs_draw.
EMSCRIPTEN_KEEPALIVE u8* thumbs_staging(u32 size) {
    thumbs.staging.assign(size, 0);
    thumbs.staging.shrink_to_fit();
    return thumbs.staging.data();
}

/// Draws the staged cache file as a `pixels`-square picture on a clear sheet, from the front right
/// and above, in a style and colour mode as engine.h numbers them. Answers through
/// thumbsHost.drawn, once the GPU has drawn it. One picture at a time.
EMSCRIPTEN_KEEPALIVE void thumbs_draw(u32 pixels, u32 style, u32 colorMode, float pixelsPerPoint,
                                      float lineScale, int firstColor) {
    if (!thumbs.renderer || thumbs.busy) {
        drawnNothing(thumbs.busy ? "A picture is being drawn." : "The renderer is not ready.");
        return;
    }
    thumbs.busy = true;
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    const bool read = scene::readCacheFile(thumbs.staging, segments, error);
    thumbs.staging = {};
    if (!read) {
        drawnNothing(error);
        return;
    }
    GeoRenderOptions options = geo_default_render_options();
    options.width = options.height = std::clamp(pixels, 16u, 2048u);
    options.overlay = false;
    options.azimuth_degrees = 35.0;
    options.elevation_degrees = 30.0;
    options.look.style = GeoStyle(std::min(style, u32(GeoStyleStudio)));
    options.look.color_mode = GeoColorMode(std::min(colorMode, u32(GeoColorFile)));
    options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
    options.pixels_per_point = pixelsPerPoint > 0.0f ? pixelsPerPoint : 2.0f;
    options.line_scale = lineScale;
    options.first_color = firstColor;
    options.transparent = true;
    thumbs.frame = std::make_unique<engine::OffscreenView>();
    if (!engine::prepareOffscreen(*thumbs.renderer, std::move(segments), options, *thumbs.frame,
                                  error)) {
        drawnNothing(error);
        return;
    }
    const u32 side = options.width;
    // The part's own pixels, from its ids, then the picture; each answer comes a frame or more
    // later.
    thumbs.renderer->requestOccurrenceIds(
        thumbs.frame->view, side, [side](render::OccurrenceIds ids) {
            const std::array<u32, 4> part =
                ids.front.size() == usize(side) * side
                    ? boundsOf(side, side,
                               [&](u32 x, u32 y) { return ids.front[usize(y) * side + x] != 0; })
                    : std::array<u32, 4>{};
            thumbs.renderer->renderOffscreenAsync(
                thumbs.frame->view, nullptr, [side, part](std::vector<u8> bgra) {
                    if (bgra.size() != usize(side) * side * 4) {
                        drawnNothing("The picture could not be read back.");
                        return;
                    }
                    // Premultiplied BGRA to the straight RGBA a canvas's ImageData holds.
                    for (usize p = 0; p < bgra.size(); p += 4) {
                        const u32 a = bgra[p + 3];
                        const u32 b = bgra[p], g = bgra[p + 1], r = bgra[p + 2];
                        const auto straight = [a](u32 c) {
                            return u8(a == 0 ? 0 : std::min(255u, (c * 255 + a / 2) / a));
                        };
                        bgra[p] = straight(r);
                        bgra[p + 1] = straight(g);
                        bgra[p + 2] = straight(b);
                    }
                    const std::array<u32, 4> ink = boundsOf(side, side, [&](u32 x, u32 y) {
                        return bgra[(usize(y) * side + x) * 4 + 3] != 0;
                    });
                    const std::array<u32, 8> rects{part[0], part[1], part[2], part[3],
                                                   ink[0],  ink[1],  ink[2],  ink[3]};
                    thumbs.busy = false;
                    jsDrawn(bgra.data(), side, side, thumbs.frame->worldPerPixel, rects.data(), "");
                });
        });
}

/// Lets go of the last file drawn, so an idle worker holds no geometry.
EMSCRIPTEN_KEEPALIVE void thumbs_trim() {
    if (!thumbs.busy && thumbs.renderer) {
        thumbs.renderer->syncScene(scene::Scene{});
        thumbs.frame.reset();
    }
}

/// The module's heap, in bytes: the worker is replaced once it grows past a limit.
EMSCRIPTEN_KEEPALIVE double thumbs_heap_bytes() {
    return double(emscripten_get_heap_size());
}

} // extern "C"
