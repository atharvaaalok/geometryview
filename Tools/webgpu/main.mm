// geometryview-webgpu: the WebGPU renderer natively on Dawn, beside the Metal one. Built with
// GEO_WEBGPU on the Mac.
//
//   geometryview-webgpu compare <metallib> <blob> <out-prefix> [options]
//       renders the file with both renderers, writes <out-prefix>-metal.png and -webgpu.png,
//       and prints how far apart they are
//   geometryview-webgpu bench <blob> [options] [--frames N] [--samples S] [--metal <metallib>]
//       times an orbit of frames drawn by the WebGPU renderer, each waited for, or with --metal
//       the same orbit drawn by the Metal renderer offscreen (its GPU time includes the readback)
//
// Options: --size W H, --style N, --perspective, --section x0.5[,z-0.3], --azimuth A,
// --elevation E.
#include "core/offscreen_view.h"
#include "engine.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "nav/camera.h"
#include "scene/segment.h"
#include "webgpu/renderer.h"

#import <Metal/Metal.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <numbers>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace geo;

namespace {

struct Dawn {
    wgpu::Instance instance;
    wgpu::Device device;
};

bool makeDawn(Dawn& dawn) {
    static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDescriptor{};
    instanceDescriptor.requiredFeatureCount = 1;
    instanceDescriptor.requiredFeatures = &kTimedWaitAny;
    dawn.instance = wgpu::CreateInstance(&instanceDescriptor);
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    wgpu::Adapter adapter;
    dawn.instance.WaitAny(
        dawn.instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                     [&](wgpu::RequestAdapterStatus, wgpu::Adapter a,
                                         wgpu::StringView) { adapter = std::move(a); }),
        UINT64_MAX);
    if (!adapter) {
        return false;
    }
    wgpu::Limits adapterLimits{};
    adapter.GetLimits(&adapterLimits);
    wgpu::Limits limits{};
    limits.maxBufferSize = adapterLimits.maxBufferSize;
    limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
    std::vector<wgpu::FeatureName> features;
    if (adapter.HasFeature(wgpu::FeatureName::TimestampQuery)) {
        features.push_back(wgpu::FeatureName::TimestampQuery);
    }
    // Dawn rounds timestamps unless asked not to, which hides short frames.
    static const char* const disabledToggles[] = {"timestamp_quantization"};
    static wgpu::DawnTogglesDescriptor toggles{};
    toggles.disabledToggleCount = 1;
    toggles.disabledToggles = disabledToggles;
    wgpu::DeviceDescriptor descriptor{};
    descriptor.nextInChain = &toggles;
    descriptor.requiredFeatureCount = features.size();
    descriptor.requiredFeatures = features.data();
    descriptor.requiredLimits = &limits;
    descriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType,
                                             wgpu::StringView message) {
        std::println(stderr, "webgpu error: {}", std::string_view(message.data, message.length));
    });
    dawn.instance.WaitAny(
        adapter.RequestDevice(&descriptor, wgpu::CallbackMode::WaitAnyOnly,
                              [&](wgpu::RequestDeviceStatus, wgpu::Device d, wgpu::StringView) {
                                  dawn.device = std::move(d);
                              }),
        UINT64_MAX);
    return bool(dawn.device);
}

std::vector<std::shared_ptr<scene::Segment>> openBlob(const char* path, std::string& error) {
    std::vector<std::shared_ptr<scene::Segment>> segments;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        error = std::string("Could not open ") + path + ".";
        return segments;
    }
    scene::openCacheFile(fd, segments, error);
    close(fd);
    return segments;
}

// Reads the options shared by both commands from `argv[first...]`.
GeoRenderOptions parseOptions(int argc, const char** argv, int first, u32& frames, u32& samples) {
    GeoRenderOptions options = geo_default_render_options();
    options.overlay = false;
    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--size" && i + 2 < argc) {
            options.width = u32(std::atoi(argv[++i]));
            options.height = u32(std::atoi(argv[++i]));
        } else if (arg == "--style" && i + 1 < argc) {
            options.look.style = GeoStyle(std::atoi(argv[++i]));
            options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
        } else if (arg == "--perspective") {
            options.perspective = true;
        } else if (arg == "--azimuth" && i + 1 < argc) {
            options.azimuth_degrees = std::atof(argv[++i]);
        } else if (arg == "--elevation" && i + 1 < argc) {
            options.elevation_degrees = std::atof(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = u32(std::atoi(argv[++i]));
        } else if (arg == "--samples" && i + 1 < argc) {
            samples = u32(std::atoi(argv[++i]));
        } else if (arg == "--section" && i + 1 < argc) {
            // Axis letters and fractions, such as "x0.5,z-0.3"; a minus keeps the other side.
            std::string_view list = argv[++i];
            while (!list.empty()) {
                const usize comma = std::min(list.find(','), list.size());
                const std::string_view part = list.substr(0, comma);
                list.remove_prefix(std::min(comma + 1, list.size()));
                if (part.size() < 2 || part[0] < 'x' || part[0] > 'z') {
                    continue;
                }
                const int axis = part[0] - 'x';
                const float value = float(std::atof(std::string(part.substr(1)).c_str()));
                options.section.enabled[axis] = true;
                options.section.flip[axis] = value < 0;
                options.section.position[axis] = std::fabs(value);
            }
        }
    }
    return options;
}

double compare(const std::vector<u8>& a, const std::vector<u8>& b, double& changed, int& worst) {
    const usize pixels = a.size() / 4;
    u64 total = 0, count = 0;
    worst = 0;
    for (usize p = 0; p < pixels; ++p) {
        int pixelWorst = 0;
        for (usize c = 0; c < 3; ++c) {
            const int delta = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
            total += u64(delta);
            pixelWorst = std::max(pixelWorst, delta);
        }
        worst = std::max(worst, pixelWorst);
        count += pixelWorst > 12 ? 1 : 0;
    }
    changed = double(count) / double(pixels);
    return double(total) / double(pixels * 3);
}

int compareCommand(int argc, const char** argv) {
    u32 frames = 0, samples = 4;
    const GeoRenderOptions options = parseOptions(argc, argv, 5, frames, samples);
    std::string error;
    std::unique_ptr<gpu::Renderer> metal =
        gpu::Renderer::create(MTLCreateSystemDefaultDevice(), argv[2]);
    Dawn dawn;
    if (!metal || !makeDawn(dawn)) {
        std::println(stderr, "No GPU for both renderers.");
        return 1;
    }
    std::unique_ptr<webgpu::Renderer> web = webgpu::Renderer::create(
        dawn.instance, dawn.device, wgpu::TextureFormat::BGRA8Unorm, error);
    if (!web) {
        std::println(stderr, "{}", error);
        return 1;
    }
    std::vector<u8> images[2];
    render::Backend* backends[2] = {metal.get(), web.get()};
    for (int r = 0; r < 2; ++r) {
        engine::OffscreenView frame;
        const auto started = std::chrono::steady_clock::now();
        if (!engine::prepareOffscreen(*backends[r], openBlob(argv[3], error), options, frame,
                                      error)) {
            std::println(stderr, "{}", error);
            return 1;
        }
        images[r] = backends[r]->renderOffscreen(frame.view, nullptr);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const std::string path = std::string(argv[4]) + (r == 0 ? "-metal.png" : "-webgpu.png");
        engine::writePng(path.c_str(), images[r], options.width, options.height);
        std::println("{}: {:.0f} ms to load and draw", r == 0 ? "metal" : "webgpu", seconds * 1e3);
    }
    double changed = 0;
    int worst = 0;
    const double mean = compare(images[0], images[1], changed, worst);
    std::println("mean {:.3f} levels, {:.3f}% of pixels more than 12 off, worst {}", mean,
                 changed * 100, worst);
    return 0;
}

// The Metal renderer's offscreen frames over the same orbit. Metal reports GPU times only while
// tracing, for its latest 2048 frames, so after 10 frames to warm up, 2048 are drawn and timed.
int benchMetal(const char* library, const char* blob, const GeoRenderOptions& options) {
    setenv("GEOMETRY_TRACE", "1", 1);
    std::string error;
    std::unique_ptr<gpu::Renderer> metal =
        gpu::Renderer::create(MTLCreateSystemDefaultDevice(), library);
    engine::OffscreenView frame;
    if (!metal || !engine::prepareOffscreen(*metal, openBlob(blob, error), options, frame, error)) {
        std::println(stderr, "{}", error);
        return 1;
    }
    const u32 frames = 2048;
    const std::optional<Aabb> bounds = frame.scene.bounds();
    const nav::Viewport viewport{double(options.width), double(options.height)};
    for (u32 f = 0; f < frames + 10; ++f) {
        const double azimuth =
            (options.azimuth_degrees + 360.0 * f / frames) * std::numbers::pi / 180.0;
        const double elevation = options.elevation_degrees * std::numbers::pi / 180.0;
        nav::Camera camera;
        camera.perspective = options.perspective;
        const Vec3 direction =
            simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                              -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
        camera.lookAt(bounds->center() + direction, bounds->center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, *bounds, viewport, 1.12);
        render::ViewState view = render::makeView(camera, viewport, *bounds);
        view.uniforms.lineOffset = frame.view.uniforms.lineOffset;
        view.uniforms.aoRadius = frame.view.uniforms.aoRadius;
        view.uniforms.aoSamples = frame.view.uniforms.aoSamples;
        (void)metal->renderOffscreen(view);
    }
    std::println("{{\"renderer\": \"metal\", \"frames\": {}, \"width\": {}, \"height\": {}, "
                 "\"triangles\": {}, \"gpu\": [{:.2f}, {:.2f}, {:.2f}]}}",
                 frames, options.width, options.height, metal->sceneTriangles(),
                 metal->gpuPercentileSeconds(0.5) * 1e3, metal->gpuPercentileSeconds(0.9) * 1e3,
                 metal->gpuPercentileSeconds(0.99) * 1e3);
    return 0;
}

int benchCommand(int argc, const char** argv) {
    u32 frames = 120, samples = 1;
    const GeoRenderOptions options = parseOptions(argc, argv, 3, frames, samples);
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--metal") {
            return benchMetal(argv[i + 1], argv[2], options);
        }
    }
    std::string error;
    Dawn dawn;
    if (!makeDawn(dawn)) {
        std::println(stderr, "No WebGPU adapter.");
        return 1;
    }
    std::unique_ptr<webgpu::Renderer> web = webgpu::Renderer::create(
        dawn.instance, dawn.device, wgpu::TextureFormat::BGRA8Unorm, error);
    if (!web) {
        std::println(stderr, "{}", error);
        return 1;
    }
    // --stream reads the file as the browser does: streamed in pieces straight to the GPU.
    bool stream = false;
    for (int i = 3; i < argc; ++i) {
        stream = stream || std::string_view(argv[i]) == "--stream";
    }
    std::vector<std::shared_ptr<scene::Segment>> segments;
    if (stream) {
        const int fd = open(argv[2], O_RDONLY);
        std::vector<u8> piece(1 << 20);
        scene::CacheStream reader(web.get(), [&](std::shared_ptr<scene::Segment> segment) {
            segments.push_back(std::move(segment));
        });
        for (ssize_t got; (got = read(fd, piece.data(), piece.size())) > 0;) {
            if (!reader.append({piece.data(), usize(got)}, error)) {
                break;
            }
        }
        close(fd);
        if (!error.empty() || !reader.finish(error)) {
            std::println(stderr, "{}", error);
            return 1;
        }
    } else {
        segments = openBlob(argv[2], error);
    }
    engine::OffscreenView frame;
    if (!engine::prepareOffscreen(*web, std::move(segments), options, frame, error)) {
        std::println(stderr, "{}", error);
        return 1;
    }
    web->setSamples(samples);
    wgpu::TextureDescriptor descriptor{};
    descriptor.size = {options.width, options.height, 1};
    descriptor.format = wgpu::TextureFormat::BGRA8Unorm;
    descriptor.usage = wgpu::TextureUsage::RenderAttachment;
    const wgpu::Texture target = dawn.device.CreateTexture(&descriptor);
    const wgpu::Queue queue = dawn.device.GetQueue();

    // An orbit about the model's centre, as prepareOffscreen frames it.
    const std::optional<Aabb> bounds = frame.scene.bounds();
    const nav::Viewport viewport{double(options.width), double(options.height)};
    std::vector<double> times, gpuTimes;
    u64 timed = web->timedFrames();
    for (u32 f = 0; f < frames + 10; ++f) {
        const double azimuth =
            (options.azimuth_degrees + 360.0 * f / frames) * std::numbers::pi / 180.0;
        const double elevation = options.elevation_degrees * std::numbers::pi / 180.0;
        nav::Camera camera;
        camera.perspective = options.perspective;
        const Vec3 direction =
            simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                              -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
        camera.lookAt(bounds->center() + direction, bounds->center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, *bounds, viewport, 1.12);
        render::ViewState view = render::makeView(camera, viewport, *bounds);
        view.uniforms.lineOffset = frame.view.uniforms.lineOffset;
        view.uniforms.aoRadius = frame.view.uniforms.aoRadius;
        view.uniforms.aoSamples = frame.view.uniforms.aoSamples;
        const auto started = std::chrono::steady_clock::now();
        web->render(target, view);
        dawn.instance.WaitAny(
            queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                                      [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {}),
            UINT64_MAX);
        if (f >= 10) { // the first frames build pipelines
            times.push_back(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() *
                1e3);
            // The previous frame's timestamps arrive while this one is encoded.
            if (web->timedFrames() != timed) {
                timed = web->timedFrames();
                gpuTimes.push_back(web->gpuMilliseconds());
            }
        }
    }
    const auto at = [](std::vector<double> values, double p) {
        if (values.empty()) {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, usize(p * double(values.size())))];
    };
    std::println("{{\"frames\": {}, \"samples\": {}, \"width\": {}, \"height\": {}, "
                 "\"triangles\": {}, \"wall\": [{:.2f}, {:.2f}, {:.2f}], "
                 "\"gpu\": [{:.2f}, {:.2f}, {:.2f}], \"timed\": {}, \"bufferMB\": {:.1f}, "
                 "\"targetMB\": {:.1f}}}",
                 times.size(), samples, options.width, options.height, web->sceneTriangles(),
                 at(times, 0.5), at(times, 0.9), at(times, 0.99), at(gpuTimes, 0.5),
                 at(gpuTimes, 0.9), at(gpuTimes, 0.99), gpuTimes.size(),
                 double(web->memoryBytes()[0]) / 1048576.0,
                 double(web->memoryBytes()[1]) / 1048576.0);
    return 0;
}

} // namespace

int main(int argc, const char** argv) {
    @autoreleasepool {
        const std::string_view command = argc > 1 ? argv[1] : "";
        if (command == "compare" && argc >= 5) {
            return compareCommand(argc, argv);
        }
        if (command == "bench" && argc >= 3) {
            return benchCommand(argc, argv);
        }
        std::println(stderr, "usage: geometryview-webgpu compare <metallib> <blob> <out-prefix> "
                             "[options] | bench <blob> [options]");
        return 2;
    }
}
