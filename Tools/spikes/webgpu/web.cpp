// The renderer spike in the browser. The page calls, in turn:
//   spike_init(width, height)            asks for a WebGPU device; spike_state() turns 1 when ready
//   spike_load(bytes, size, copies, indexed)  builds and uploads the scene from a cache file in
//                                        memory, to draw pulled or indexed (see Mode)
//   spike_start()                        runs the scripted orbit; spike_state() turns 3 when done
//   spike_results()                      the timings, as JSON
// A negative state is an error, which spike_error() describes.

#include "renderer.h"
#include "scene.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

using namespace geo;
using namespace geo::spike;

namespace {

constexpr double kWarmupMs = 2000;
constexpr double kPhaseMs = 6000;
constexpr float kZooms[] = {1.f, 2.f, 4.f};

struct App {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    Renderer renderer;
    u64 triangles = 0, instances = 0;
    u32 width = 0, height = 0;
    int state = 0;
    std::string error, results, adapterName;

    int phase = -1; // -1 warming up, then one per zoom
    double phaseStart = 0, lastFrame = 0;
    std::vector<double> intervals[3], cpu[3];
    std::vector<FrameTiming> timings[3];
};

App app;

void fail(std::string message) {
    app.error = std::move(message);
    app.state = -1;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, usize(p * double(values.size())))];
}

void collect() {
    for (const FrameTiming& t : app.renderer.timings()) {
        if (t.tag >= 1 && t.tag <= 3) {
            app.timings[t.tag - 1].push_back(t);
        }
    }
    app.renderer.timings().clear();
}

void finish() {
    collect();
    std::string out = std::format(
        "{{\"adapter\": \"{}\", \"width\": {}, \"height\": {}, \"triangles\": {}, "
        "\"instances\": {}, \"timestamps\": {}, \"zooms\": [",
        app.adapterName, app.width, app.height, app.triangles, app.instances,
        app.renderer.timestamps());
    for (u32 z = 0; z < 3; ++z) {
        std::vector<double> total, clusters;
        for (const FrameTiming& t : app.timings[z]) {
            total.push_back(t.total);
            clusters.push_back(double(t.clusters));
        }
        out += std::format(
            "{}{{\"zoom\": {}, \"frames\": {}, \"timedFrames\": {}, "
            "\"gpuMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, "
            "\"frameIntervalMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, "
            "\"cpuSubmitMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, \"clustersP50\": {:.0f}}}",
            z ? ", " : "", kZooms[z], app.intervals[z].size(), total.size(),
            percentile(total, 0.5), percentile(total, 0.99), percentile(app.intervals[z], 0.5),
            percentile(app.intervals[z], 0.99), percentile(app.cpu[z], 0.5),
            percentile(app.cpu[z], 0.99), percentile(clusters, 0.5));
    }
    app.results = out + "]}";
    app.state = 3;
}

bool frame(double now, void*) {
    if (app.phase == -1 && app.phaseStart == 0) {
        app.phaseStart = now;
    }
    const double elapsed = now - app.phaseStart;
    if ((app.phase == -1 && elapsed >= kWarmupMs) || (app.phase >= 0 && elapsed >= kPhaseMs)) {
        ++app.phase;
        app.phaseStart = now;
        app.lastFrame = 0;
        if (app.phase == 3) {
            finish();
            return false;
        }
    }
    const int zoom = std::max(app.phase, 0);
    const float turn = float((now - app.phaseStart) / kPhaseMs);
    const View view{35.f + 360.f * turn, 25.f, kZooms[zoom]};
    if (app.phase >= 0 && app.lastFrame != 0) {
        app.intervals[app.phase].push_back(now - app.lastFrame);
    }
    app.lastFrame = now;

    wgpu::SurfaceTexture surfaceTexture;
    app.surface.GetCurrentTexture(&surfaceTexture);
    const double started = emscripten_get_now();
    app.renderer.setTag(u32(app.phase + 1));
    app.renderer.frame(view, surfaceTexture.texture.CreateView());
    if (app.phase >= 0) {
        app.cpu[app.phase].push_back(emscripten_get_now() - started);
    }
    collect();
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
    app.state = 1;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void spike_init(u32 width, u32 height) {
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
            wgpu::DeviceDescriptor descriptor{};
            descriptor.requiredFeatureCount = features.size();
            descriptor.requiredFeatures = features.data();
            descriptor.requiredLimits = &limits;
            descriptor.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView text) {
                    emscripten_log(EM_LOG_ERROR, "webgpu: %.*s", int(text.length), text.data);
                });
            app.adapter.RequestDevice(
                &descriptor, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView text) {
                    if (status != wgpu::RequestDeviceStatus::Success) {
                        fail(std::string(text.data, text.length));
                        return;
                    }
                    app.device = std::move(device);
                    deviceReady();
                });
        });
}

EMSCRIPTEN_KEEPALIVE int spike_state() { return app.state; }

EMSCRIPTEN_KEEPALIVE const char* spike_error() { return app.error.c_str(); }

EMSCRIPTEN_KEEPALIVE int spike_load(const u8* bytes, u32 size, u32 copies, u32 indexed) {
    SceneData scene;
    std::string error;
    const Mode mode = indexed != 0 ? Mode::Indexed : Mode::Pulled;
    if (!buildScene({bytes, size}, copies, scene, error) ||
        !app.renderer.init(app.device, scene, mode, app.format,
                           wgpu::CallbackMode::AllowSpontaneous, error)) {
        fail(error);
        return -1;
    }
    app.renderer.resize(app.width, app.height);
    app.triangles = scene.triangles;
    app.instances = scene.instances.size();
    app.state = 2;
    return 0;
}

EMSCRIPTEN_KEEPALIVE void spike_start() { emscripten_request_animation_frame_loop(frame, nullptr); }

EMSCRIPTEN_KEEPALIVE const char* spike_results() { return app.results.c_str(); }

} // extern "C"
