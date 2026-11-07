// The renderer spike natively on Dawn: the control that separates WebGPU's own cost from a
// browser's. Renders a scripted orbit offscreen at three zooms and prints GPU timings as JSON.
//
//   spike_webgpu <blob> [--size W H] [--frames N] [--copies C] [--png out.ppm]

#include "renderer.h"
#include "scene.h"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace geo;
using namespace geo::spike;

namespace {

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, usize(p * double(values.size())))];
}

void writePpm(const wgpu::Instance& instance, const wgpu::Device& device,
              const wgpu::Texture& texture, u32 width, u32 height, const char* path) {
    const u32 stride = (width * 4 + 255) & ~255u;
    wgpu::BufferDescriptor descriptor{};
    descriptor.size = u64(stride) * height;
    descriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    const wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
    wgpu::TexelCopyTextureInfo source{};
    source.texture = texture;
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = buffer;
    destination.layout.bytesPerRow = stride;
    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    const wgpu::Extent3D extent = {width, height, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &extent);
    const wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    instance.WaitAny(buffer.MapAsync(wgpu::MapMode::Read, 0, descriptor.size,
                                     wgpu::CallbackMode::WaitAnyOnly,
                                     [](wgpu::MapAsyncStatus, wgpu::StringView) {}),
                     UINT64_MAX);
    const auto* bytes = static_cast<const u8*>(buffer.GetConstMappedRange(0, descriptor.size));
    FILE* file = std::fopen(path, "wb");
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            std::fwrite(bytes + y * stride + x * 4, 1, 3, file);
        }
    }
    std::fclose(file);
    buffer.Unmap();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: spike_webgpu <blob> [--size W H] [--frames N] [--copies C] "
                             "[--png out.ppm]");
        return 2;
    }
    u32 width = 2880, height = 1800, frames = 360, copies = 1;
    double interval = 0; // milliseconds between frame starts, as a display would pace them
    const char* png = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 2 < argc) {
            width = u32(std::atoi(argv[++i]));
            height = u32(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = u32(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--copies") && i + 1 < argc) {
            copies = u32(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--interval") && i + 1 < argc) {
            interval = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--png") && i + 1 < argc) {
            png = argv[++i];
        }
    }

    const int fd = open(argv[1], O_RDONLY);
    struct stat info {};
    if (fd < 0 || fstat(fd, &info) != 0) {
        std::println(stderr, "cannot open {}", argv[1]);
        return 1;
    }
    const usize size = usize(info.st_size);
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    SceneData scene;
    std::string error;
    if (mapped == MAP_FAILED ||
        !buildScene({static_cast<const u8*>(mapped), size}, copies, scene, error)) {
        std::println(stderr, "invalid: {}", error);
        return 1;
    }

    static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDescriptor{};
    instanceDescriptor.requiredFeatureCount = 1;
    instanceDescriptor.requiredFeatures = &kTimedWaitAny;
    const wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);

    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    wgpu::Adapter adapter;
    instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus, wgpu::Adapter a,
                                                 wgpu::StringView) { adapter = std::move(a); }),
                     UINT64_MAX);
    if (!adapter) {
        std::println(stderr, "no adapter");
        return 1;
    }
    wgpu::AdapterInfo adapterInfo{};
    adapter.GetInfo(&adapterInfo);
    wgpu::Limits adapterLimits{};
    adapter.GetLimits(&adapterLimits);

    std::vector<wgpu::FeatureName> features;
    if (adapter.HasFeature(wgpu::FeatureName::TimestampQuery)) {
        features.push_back(wgpu::FeatureName::TimestampQuery);
    }
    wgpu::Limits limits{};
    limits.maxBufferSize = adapterLimits.maxBufferSize;
    limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
    // Dawn rounds timestamps to 65.5 µs unless asked not to, which hides short passes.
    const char* const disabledToggles[] = {"timestamp_quantization"};
    wgpu::DawnTogglesDescriptor toggles{};
    toggles.disabledToggleCount = 1;
    toggles.disabledToggles = disabledToggles;
    wgpu::DeviceDescriptor deviceDescriptor{};
    deviceDescriptor.nextInChain = &toggles;
    deviceDescriptor.requiredFeatureCount = features.size();
    deviceDescriptor.requiredFeatures = features.data();
    deviceDescriptor.requiredLimits = &limits;
    deviceDescriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
            std::println(stderr, "webgpu error: {}", std::string_view(message.data, message.length));
        });
    wgpu::Device device;
    instance.WaitAny(adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestDeviceStatus, wgpu::Device d,
                                               wgpu::StringView) { device = std::move(d); }),
                     UINT64_MAX);
    if (!device) {
        std::println(stderr, "no device");
        return 1;
    }

    Renderer renderer;
    if (!renderer.init(device, scene, wgpu::TextureFormat::RGBA8Unorm,
                       wgpu::CallbackMode::AllowProcessEvents, error)) {
        std::println(stderr, "{}", error);
        return 1;
    }
    munmap(mapped, size); // uploaded: the GPU holds the only copy now
    renderer.resize(width, height);

    wgpu::TextureDescriptor targetDescriptor{};
    targetDescriptor.size = {width, height, 1};
    targetDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    targetDescriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    const wgpu::Texture target = device.CreateTexture(&targetDescriptor);
    const wgpu::TextureView targetView = target.CreateView();
    const wgpu::Queue queue = device.GetQueue();

    // One frame at a time, waiting for each, so readbacks never fall behind. With an interval,
    // frames start on a fixed beat, as a display paces them, which lets GPU clocks settle lower.
    auto next = std::chrono::steady_clock::now();
    auto render = [&](const View& view) {
        if (interval > 0) {
            next += std::chrono::microseconds(i64(interval * 1000));
            std::this_thread::sleep_until(next);
        }
        renderer.frame(view, targetView);
        instance.WaitAny(queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                                                   [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {}),
                         UINT64_MAX);
        instance.ProcessEvents();
    };
    for (u32 i = 0; i < 30; ++i) {
        render({});
    }
    instance.ProcessEvents();
    renderer.timings().clear();
    if (png != nullptr) {
        render({});
        writePpm(instance, device, target, width, height, png);
        renderer.timings().clear();
    }

    std::println("{{\"adapter\": \"{}\", \"backend\": \"Dawn native\", \"width\": {}, \"height\": {}, "
                 "\"intervalMs\": {},",
                 std::string_view(adapterInfo.device.data, adapterInfo.device.length), width, height,
                 interval);
    std::println(" \"triangles\": {}, \"instances\": {}, \"timestamps\": {}, \"zooms\": [",
                 scene.triangles, scene.instances.size(), renderer.timestamps());
    const float zooms[] = {1.f, 2.f, 4.f};
    for (u32 z = 0; z < 3; ++z) {
        for (u32 f = 0; f < frames; ++f) {
            render({35.f + 360.f * float(f) / float(frames), 25.f, zooms[z]});
        }
        instance.ProcessEvents();
        std::vector<double> cull, vis, composite, total, clusters;
        for (const FrameTiming& t : renderer.timings()) {
            cull.push_back(t.cull);
            vis.push_back(t.vis);
            composite.push_back(t.composite);
            total.push_back(t.total);
            clusters.push_back(double(t.clusters));
        }
        renderer.timings().clear();
        std::println("  {{\"zoom\": {}, \"frames\": {}, \"gpuMs\": {{\"p50\": {:.3f}, \"p99\": {:.3f}}}, "
                     "\"cullMs\": {:.3f}, \"visMs\": {:.3f}, \"compositeMs\": {:.3f}, "
                     "\"clustersP50\": {:.0f}}}{}",
                     zooms[z], total.size(), percentile(total, 0.5), percentile(total, 0.99),
                     percentile(cull, 0.5), percentile(vis, 0.5), percentile(composite, 0.5),
                     percentile(clusters, 0.5), z < 2 ? "," : "");
    }
    std::println("]}}");
    return 0;
}
