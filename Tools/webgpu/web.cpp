// The WebGPU renderer in the browser, timed: what the Phase 0 spike measured, now with the
// viewer's own renderer. The page calls, in turn:
//   bench_init(width, height)                 asks for a device; bench_state() turns 1 when ready
//   bench_load(bytes, size, style, samples)   builds the scene from a cache file in memory
//   bench_start(burst, phaseSeconds)          orbits for `phaseSeconds` at each of three zooms,
//                                             `burst` frames per display refresh, with a hover
//                                             sweep; bench_state() turns 3 when done
//   bench_results()                           the timings, as JSON
// A negative state is an error, which bench_error() describes.
#include "core/offscreen_view.h"
#include "nav/camera.h"
#include "scene/segment.h"
#include "webgpu/renderer.h"
#include "webgpu/wgsl.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <vector>

using namespace geo;

namespace {

constexpr double kWarmupMs = 2000;
constexpr double kZooms[] = {1.0, 2.0, 4.0};

struct App {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    std::unique_ptr<webgpu::Renderer> renderer;
    engine::OffscreenView frame;
    Aabb bounds{};
    u32 width = 0, height = 0;
    int state = 0;
    std::string error, results, adapterName;
    int phase = -1; // -1 warming up, then one per zoom
    u32 burst = 1;
    double phaseMs = 6000;
    double phaseStart = 0, lastFrame = 0;
    u64 timed = 0;
    std::vector<double> intervals[3], cpu[3], gpu[3];
    u32 picks[3] = {}, hits[3] = {};
    std::vector<u8> capture; // a frame's pixels, for checking the image
    std::string diagnostics; // what the browser's WGSL compiler and the first frame said
};

App app;

// Keeps the first error, which explains the ones that follow it.
void fail(std::string message) {
    if (app.error.empty()) {
        app.error = std::move(message);
    }
    app.state = -1;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, usize(p * double(values.size())))];
}

void finish() {
    std::string out = std::format(
        "{{\"adapter\": \"{}\", \"width\": {}, \"height\": {}, \"triangles\": {}, "
        "\"edgeSegments\": {}, \"probes\": {}, \"probesFailed\": {}, \"probeIds\": {}, "
        "\"probeDepths\": {}, \"bufferMB\": {:.1f}, \"targetMB\": {:.1f}, \"zooms\": [",
        app.adapterName, app.width, app.height, app.renderer->sceneTriangles(),
        app.renderer->sceneEdgeSegments(), app.renderer->probeCounts()[0],
        app.renderer->probeCounts()[1], app.renderer->probeContents()[0],
        app.renderer->probeContents()[1], double(app.renderer->memoryBytes()[0]) / 1048576.0,
        double(app.renderer->memoryBytes()[1]) / 1048576.0);
    for (u32 z = 0; z < 3; ++z) {
        out += std::format(
            "{}{{\"zoom\": {}, \"frames\": {}, \"timedFrames\": {}, "
            "\"gpuMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, "
            "\"frameIntervalMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, "
            "\"cpuSubmitMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, \"picks\": {}, "
            "\"pickHits\": {}}}",
            z ? ", " : "", kZooms[z], app.intervals[z].size(), app.gpu[z].size(),
            percentile(app.gpu[z], 0.5), percentile(app.gpu[z], 0.99),
            percentile(app.intervals[z], 0.5), percentile(app.intervals[z], 0.99),
            percentile(app.cpu[z], 0.5), percentile(app.cpu[z], 0.99), app.picks[z],
            app.hits[z]);
    }
    std::string escaped;
    for (const char c : app.diagnostics) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c == '\n' ? ' ' : c;
    }
    app.results = out + "], \"diagnostics\": \"" + escaped + "\"}";
    app.state = 3;
}

// The orbit at a time and zoom: a turn every 6 s about the model's centre.
render::ViewState viewAt(double turn, double zoom) {
    const nav::Viewport viewport{double(app.width), double(app.height)};
    const double azimuth = (35.0 + 360.0 * turn) * std::numbers::pi / 180.0;
    const double elevation = 25.0 * std::numbers::pi / 180.0;
    const Vec3 direction =
        simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                          -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    nav::Camera camera;
    camera.lookAt(app.bounds.center() + direction, app.bounds.center(),
                  simd_make_double3(0.0, 0.0, 1.0));
    nav::fit(camera, app.bounds, viewport, 1.12);
    nav::zoomAbout(camera, 1.0 / zoom, app.bounds.center(), 0.0, 0.0, viewport);
    render::ViewState view = render::makeView(camera, viewport, app.bounds);
    view.uniforms.edgeFloor = app.frame.view.uniforms.edgeFloor;
    view.uniforms.aoRadius = app.frame.view.uniforms.aoRadius;
    view.uniforms.aoSamples = app.frame.view.uniforms.aoSamples;
    return view;
}

bool frame(double now, void*) {
    if (app.phase == -1 && app.phaseStart == 0) {
        app.phaseStart = now;
    }
    const double elapsed = now - app.phaseStart;
    if ((app.phase == -1 && elapsed >= kWarmupMs) || (app.phase >= 0 && elapsed >= app.phaseMs)) {
        ++app.phase;
        app.phaseStart = now;
        app.lastFrame = 0;
        if (app.phase == 3) {
            finish();
            return false;
        }
    }
    const int zoom = std::max(app.phase, 0);
    const double turn = (now - app.phaseStart) / 6000.0;
    const render::ViewState view = viewAt(turn, kZooms[zoom]);
    // A hover sweep across the middle of the view, at most about 1000 pixels a second, as a
    // brisk pointer moves.
    const int x = int(double(app.width) * (0.5 + 0.35 * std::sin(turn * 6.0)));
    const int y = int(app.height / 2);
    const std::optional<render::PixelHit> hit = app.renderer->readPixel(x, y, view.depth);
    if (app.phase >= 0) {
        ++app.picks[app.phase];
        app.hits[app.phase] += hit ? 1 : 0;
        if (app.lastFrame != 0) {
            app.intervals[app.phase].push_back(now - app.lastFrame);
        }
        if (app.renderer->timedFrames() != app.timed) {
            app.gpu[app.phase].push_back(app.renderer->gpuMilliseconds());
        }
    }
    app.timed = app.renderer->timedFrames();
    app.lastFrame = now;
    wgpu::SurfaceTexture surfaceTexture;
    app.surface.GetCurrentTexture(&surfaceTexture);
    const double started = emscripten_get_now();
    static int scoped = 0;
    const bool scope = scoped++ < 3; // the first frames build every pipeline they use
    if (scope) {
        app.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    }
    for (u32 i = 0; i < app.burst; ++i) {
        app.renderer->render(surfaceTexture.texture, view);
    }
    if (scope) {
        app.device.PopErrorScope(
            wgpu::CallbackMode::AllowSpontaneous,
            [](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
                if (type != wgpu::ErrorType::NoError) {
                    app.diagnostics +=
                        "frame: " + std::string(message.data, message.length) + "; ";
                }
            });
    }
    if (app.phase >= 0) {
        app.cpu[app.phase].push_back(emscripten_get_now() - started);
    }
    return true;
}

void deviceReady() {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.selector = "#canvas";
    wgpu::SurfaceDescriptor surfaceDescriptor{};
    surfaceDescriptor.nextInChain = &canvas;
    app.surface = app.instance.CreateSurface(&surfaceDescriptor);
    wgpu::SurfaceCapabilities capabilities;
    app.surface.GetCapabilities(app.adapter, &capabilities);
    app.format = capabilities.formats[0];
    wgpu::SurfaceConfiguration configuration{};
    configuration.device = app.device;
    configuration.format = app.format;
    configuration.width = app.width;
    configuration.height = app.height;
    configuration.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    app.surface.Configure(&configuration);
    // Each module through the browser's own WGSL compiler, whose errors would otherwise only
    // make later pipelines invalid.
    const std::string common = webgpu::wgsl::kCommon;
    const std::pair<const char*, const char*> modules[] = {
        {"cull", webgpu::wgsl::kCull},   {"surface", webgpu::wgsl::kSurface},
        {"edges", webgpu::wgsl::kEdges}, {"sheet", webgpu::wgsl::kSheet},
        {"post", webgpu::wgsl::kPost}};
    for (const auto& [name, source] : modules) {
        static std::vector<std::string> codes;
        codes.push_back(common + source);
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = codes.back().c_str();
        wgpu::ShaderModuleDescriptor moduleDescriptor{};
        moduleDescriptor.nextInChain = &wgsl;
        const wgpu::ShaderModule module = app.device.CreateShaderModule(&moduleDescriptor);
        const std::string label = name;
        module.GetCompilationInfo(
            wgpu::CallbackMode::AllowSpontaneous,
            [label](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* info) {
                for (usize m = 0; info != nullptr && m < info->messageCount; ++m) {
                    const wgpu::CompilationMessage& message = info->messages[m];
                    if (message.type == wgpu::CompilationMessageType::Error) {
                        app.diagnostics += std::format(
                            "{} {}:{}: {}; ", label, message.lineNum, message.linePos,
                            std::string_view(message.message.data, message.message.length));
                    }
                }
            });
    }
    std::string error;
    app.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    app.renderer = webgpu::Renderer::create(app.instance, app.device, app.format, error);
    app.device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous,
                             [](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                                wgpu::StringView message) {
                                 if (type != wgpu::ErrorType::NoError) {
                                     app.diagnostics += "creating: " +
                                                        std::string(message.data, message.length) +
                                                        "; ";
                                 }
                             });
    if (!app.renderer) {
        fail(error);
        return;
    }
    app.state = 1;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void bench_init(u32 width, u32 height) {
    app.width = width;
    app.height = height;
    app.instance = wgpu::CreateInstance(nullptr);
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    app.instance.RequestAdapter(
        &options, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                fail(std::string(message.data, message.length));
                return;
            }
            app.adapter = std::move(adapter);
            wgpu::AdapterInfo info{};
            app.adapter.GetInfo(&info);
            app.adapterName = std::string(info.vendor.data, info.vendor.length) + " " +
                              std::string(info.architecture.data, info.architecture.length);
            wgpu::Limits adapterLimits{};
            app.adapter.GetLimits(&adapterLimits);
            static std::vector<wgpu::FeatureName> features;
            if (app.adapter.HasFeature(wgpu::FeatureName::TimestampQuery)) {
                features.push_back(wgpu::FeatureName::TimestampQuery);
            }
            static wgpu::Limits limits{};
            limits.maxBufferSize = adapterLimits.maxBufferSize;
            limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
            static wgpu::DeviceDescriptor descriptor{};
            descriptor.requiredFeatureCount = features.size();
            descriptor.requiredFeatures = features.data();
            descriptor.requiredLimits = &limits;
            descriptor.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView error) {
                    fail(std::string(error.data, error.length));
                });
            app.adapter.RequestDevice(
                &descriptor, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus deviceStatus, wgpu::Device device,
                   wgpu::StringView deviceMessage) {
                    if (deviceStatus != wgpu::RequestDeviceStatus::Success) {
                        fail(std::string(deviceMessage.data, deviceMessage.length));
                        return;
                    }
                    app.device = std::move(device);
                    deviceReady();
                });
        });
}

EMSCRIPTEN_KEEPALIVE int bench_state() { return app.state; }
EMSCRIPTEN_KEEPALIVE const char* bench_error() { return app.error.c_str(); }
EMSCRIPTEN_KEEPALIVE const char* bench_results() { return app.results.c_str(); }

EMSCRIPTEN_KEEPALIVE int bench_load(const u8* bytes, u32 size, u32 style, u32 samples) {
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    if (!scene::readCacheFile({bytes, size}, segments, error)) {
        fail(error);
        return -1;
    }
    GeoRenderOptions options = geo_default_render_options();
    options.width = app.width;
    options.height = app.height;
    options.overlay = false;
    options.look.style = GeoStyle(std::min(style, u32(GeoStyleStudio)));
    options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
    if (!engine::prepareOffscreen(*app.renderer, std::move(segments), options, app.frame, error)) {
        fail(error);
        return -1;
    }
    app.bounds = app.frame.scene.bounds().value_or(Aabb{});
    app.renderer->setSamples(samples);
    app.state = 2;
    return 0;
}

// Renders one frame of the orbit's start into a texture that can be read, and reads it back;
// bench_state() turns 4 when bench_capture_bytes() holds its pixels, in the surface's format.
EMSCRIPTEN_KEEPALIVE void bench_capture() {
    wgpu::TextureDescriptor descriptor{};
    descriptor.size = {app.width, app.height, 1};
    descriptor.format = app.format;
    descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    const wgpu::Texture target = app.device.CreateTexture(&descriptor);
    app.renderer->render(target, viewAt(0.0, 1.0));
    const u32 stride = (app.width * 4 + 255) & ~255u;
    wgpu::BufferDescriptor bufferDescriptor{};
    bufferDescriptor.size = u64(stride) * app.height;
    bufferDescriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    static wgpu::Buffer readback;
    readback = app.device.CreateBuffer(&bufferDescriptor);
    wgpu::TexelCopyTextureInfo source{};
    source.texture = target;
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = readback;
    destination.layout.bytesPerRow = stride;
    const wgpu::Extent3D extent = {app.width, app.height, 1};
    const wgpu::CommandEncoder encoder = app.device.CreateCommandEncoder();
    encoder.CopyTextureToBuffer(&source, &destination, &extent);
    const wgpu::CommandBuffer commands = encoder.Finish();
    app.device.GetQueue().Submit(1, &commands);
    readback.MapAsync(wgpu::MapMode::Read, 0, usize(bufferDescriptor.size),
                      wgpu::CallbackMode::AllowSpontaneous,
                      [stride](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                          if (status != wgpu::MapAsyncStatus::Success) {
                              fail(std::string(message.data, message.length));
                              return;
                          }
                          const auto* bytes = static_cast<const u8*>(
                              readback.GetConstMappedRange(0, usize(stride) * app.height));
                          app.capture.resize(usize(app.width) * app.height * 4);
                          for (u32 y = 0; y < app.height; ++y) {
                              std::copy_n(bytes + usize(y) * stride, app.width * 4,
                                          app.capture.data() + usize(y) * app.width * 4);
                          }
                          readback.Unmap();
                          app.state = 4;
                      });
}

EMSCRIPTEN_KEEPALIVE const u8* bench_capture_bytes() { return app.capture.data(); }
EMSCRIPTEN_KEEPALIVE u32 bench_format() { return u32(app.format); }

// Edges and the cast shadow left out, as the engine draws heavy scenes in motion.
EMSCRIPTEN_KEEPALIVE void bench_reduced(int reduced) { app.renderer->setReducedDetail(reduced != 0); }

EMSCRIPTEN_KEEPALIVE void bench_start(u32 burst, double phaseSeconds) {
    app.burst = std::max(burst, 1u);
    app.phaseMs = phaseSeconds * 1000.0;
    emscripten_request_animation_frame_loop(frame, nullptr);
}

} // extern "C"
