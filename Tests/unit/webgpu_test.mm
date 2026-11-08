// The WebGPU renderer against the Metal one: the golden scenes, rendered by both from the same
// view, must match as the goldens must match Metal. Built only with GEO_WEBGPU, natively on Dawn.
// Set GEOMETRY_WEBGPU_IMAGES to a folder to keep both images of each scene.
#if defined(GEO_HAS_WEBGPU)
#include "core/offscreen_view.h"
#include "drive_fixture.h"
#include "engine.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "scene/segment.h"
#include "webgpu/renderer.h"
#include "webgpu/wgsl.h"

#import <Metal/Metal.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <format>
#include <unistd.h>
#include <webgpu/webgpu_cpp.h>

using namespace geo;

#if defined(GEO_TEST_WORKER_PATH)

namespace {

constexpr u32 kWidth = 480;
constexpr u32 kHeight = 300;

struct Dawn {
    wgpu::Instance instance;
    wgpu::Device device;
};

// A Dawn device with the adapter's buffer limits, as the viewer asks for.
Dawn makeDawn() {
    static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor instanceDescriptor{};
    instanceDescriptor.requiredFeatureCount = 1;
    instanceDescriptor.requiredFeatures = &kTimedWaitAny;
    Dawn dawn;
    dawn.instance = wgpu::CreateInstance(&instanceDescriptor);
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    wgpu::Adapter adapter;
    dawn.instance.WaitAny(
        dawn.instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                     [&](wgpu::RequestAdapterStatus, wgpu::Adapter a,
                                         wgpu::StringView) { adapter = std::move(a); }),
        UINT64_MAX);
    REQUIRE(adapter);
    wgpu::Limits adapterLimits{};
    adapter.GetLimits(&adapterLimits);
    wgpu::Limits limits{};
    limits.maxBufferSize = adapterLimits.maxBufferSize;
    limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
    wgpu::DeviceDescriptor descriptor{};
    descriptor.requiredLimits = &limits;
    descriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
            FAIL(std::string_view(message.data, message.length));
        });
    dawn.instance.WaitAny(
        adapter.RequestDevice(&descriptor, wgpu::CallbackMode::WaitAnyOnly,
                              [&](wgpu::RequestDeviceStatus, wgpu::Device d, wgpu::StringView) {
                                  dawn.device = std::move(d);
                              }),
        UINT64_MAX);
    REQUIRE(dawn.device);
    return dawn;
}

std::vector<std::shared_ptr<scene::Segment>> openBlob(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    const bool opened = scene::openCacheFile(fd, segments, error);
    close(fd);
    REQUIRE_MESSAGE(opened, error);
    return segments;
}

std::vector<u8> renderWith(render::Backend& backend, const std::string& blob,
                       const GeoRenderOptions& options) {
    engine::OffscreenView frame;
    std::string error;
    REQUIRE_MESSAGE(engine::prepareOffscreen(backend, openBlob(blob), options, frame, error),
                    error);
    return backend.renderOffscreen(frame.view, nullptr);
}

struct Difference {
    double mean = 0.0;    // mean absolute channel difference
    double changed = 0.0; // share of pixels with a channel more than 12 levels off
};

Difference compare(const std::vector<u8>& a, const std::vector<u8>& b) {
    Difference d;
    const usize pixels = a.size() / 4;
    u64 total = 0, changed = 0;
    for (usize p = 0; p < pixels; ++p) {
        int worst = 0;
        for (usize c = 0; c < 3; ++c) {
            const int delta = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
            total += u64(delta);
            worst = std::max(worst, delta);
        }
        changed += worst > 12 ? 1 : 0;
    }
    d.mean = double(total) / double(pixels * 3);
    d.changed = double(changed) / double(pixels);
    return d;
}

} // namespace

TEST_CASE("every WGSL module compiles") {
    Dawn dawn = makeDawn();
    const std::string common = webgpu::wgsl::kCommon;
    const std::pair<const char*, const char*> modules[] = {
        {"cull", webgpu::wgsl::kCull},   {"surface", webgpu::wgsl::kSurface},
        {"edges", webgpu::wgsl::kEdges}, {"sheet", webgpu::wgsl::kSheet},
        {"post", webgpu::wgsl::kPost}};
    for (const auto& [name, source] : modules) {
        CAPTURE(name);
        const std::string code = common + source;
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = code.c_str();
        wgpu::ShaderModuleDescriptor descriptor{};
        descriptor.nextInChain = &wgsl;
        dawn.device.PushErrorScope(wgpu::ErrorFilter::Validation);
        const wgpu::ShaderModule module = dawn.device.CreateShaderModule(&descriptor);
        std::string messages;
        dawn.instance.WaitAny(
            module.GetCompilationInfo(
                wgpu::CallbackMode::WaitAnyOnly,
                [&](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* info) {
                    for (usize m = 0; info != nullptr && m < info->messageCount; ++m) {
                        const wgpu::CompilationMessage& message = info->messages[m];
                        if (message.type == wgpu::CompilationMessageType::Error) {
                            messages += std::format(
                                "{}:{}: {}\n", message.lineNum, message.linePos,
                                std::string_view(message.message.data, message.message.length));
                        }
                    }
                }),
            UINT64_MAX);
        dawn.instance.WaitAny(dawn.device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                                                        [](wgpu::PopErrorScopeStatus,
                                                           wgpu::ErrorType, wgpu::StringView) {}),
                              UINT64_MAX);
        CHECK_MESSAGE(messages.empty(), messages);
    }
}

// Reads fields of the uniforms back through WGSL, so a layout that drifts from C++ shows.
TEST_CASE("the WGSL uniforms are laid out as the C++ ones") {
    Dawn dawn = makeDawn();
    const std::string code = std::string(webgpu::wgsl::kCommon) + R"wgsl(
@group(1) @binding(0) var<storage, read_write> fields: array<f32, 17>;
@compute @workgroup_size(1)
fn readFields() {
    fields[0] = frame.halfWidth;
    fields[1] = f32(frame.hoverHides);
    fields[2] = frame.viewport.y;
    fields[3] = f32(frame.aoSamples);
    fields[4] = frame.lightTravel.z;
    fields[5] = frame.ground;
    fields[6] = frame.shadowVisibility;
    fields[7] = frame.sectionPlanes[5].w;
    fields[8] = f32(frame.excludeShells);
    fields[9] = frame.forward.y;
    fields[10] = frame.viewRotation[2].z;
    fields[11] = f32(style.selection.w);
    fields[12] = style.strips.y;
    fields[13] = style.capInk;
    fields[14] = style.fillLight.z;
    fields[15] = f32(style.shadowOn);
    fields[16] = frame.sampleOffset.y;
}
)wgsl";
    FrameUniforms frame{};
    frame.halfWidth = 1.5f;
    frame.hoverHides = 7;
    frame.viewport = simd_make_float2(10.0f, 20.0f);
    frame.aoSamples = 12;
    frame.lightTravel = simd_make_float3(0.1f, 0.2f, -0.9f);
    frame.ground = -3.25f;
    frame.shadowVisibility = 0.75f;
    frame.sectionPlanes[5] = simd_make_float4(0, 0, 1, 4.5f);
    frame.excludeShells = 1;
    frame.sampleOffset = simd_make_float2(-0.125f, 0.375f);
    frame.forward = simd_make_float3(0.0f, 0.6f, -0.8f);
    frame.viewRotation.columns[2] = simd_make_float3(0.0f, 0.0f, 0.5f);
    StyleUniforms style{};
    style.selection = simd_make_uint4(1, 2, 3, 9);
    style.strips = simd_make_float2(0.3f, 0.4f);
    style.capInk = 0.25f;
    style.fill = simd_make_float3(0.0f, 0.0f, 0.125f);
    style.shadowOn = 1;
    const float expected[17] = {1.5f, 7, 20, 12, -0.9f, -3.25f, 0.75f, 4.5f, 1, 0.6f, 0.5f,
                                9, 0.4f, 0.25f, 0.125f, 1, 0.375f};

    const wgpu::Device& device = dawn.device;
    const auto makeBuffer = [&](u64 size, wgpu::BufferUsage usage) {
        wgpu::BufferDescriptor d{};
        d.size = size;
        d.usage = usage;
        return device.CreateBuffer(&d);
    };
    using U = wgpu::BufferUsage;
    const wgpu::Buffer frameBuffer = makeBuffer(sizeof(frame), U::Uniform | U::CopyDst);
    const wgpu::Buffer styleBuffer = makeBuffer(sizeof(style), U::Uniform | U::CopyDst);
    const wgpu::Buffer out = makeBuffer(68, U::Storage | U::CopySrc);
    const wgpu::Buffer readback = makeBuffer(68, U::MapRead | U::CopyDst);
    device.GetQueue().WriteBuffer(frameBuffer, 0, &frame, sizeof(frame));
    device.GetQueue().WriteBuffer(styleBuffer, 0, &style, sizeof(style));

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    wgpu::ShaderModuleDescriptor moduleDescriptor{};
    moduleDescriptor.nextInChain = &wgsl;
    wgpu::ComputePipelineDescriptor pipelineDescriptor{};
    pipelineDescriptor.compute.module = device.CreateShaderModule(&moduleDescriptor);
    pipelineDescriptor.compute.entryPoint = "readFields";
    const wgpu::ComputePipeline pipeline = device.CreateComputePipeline(&pipelineDescriptor);
    const auto group = [&](u32 index, std::vector<wgpu::BindGroupEntry> entries) {
        wgpu::BindGroupDescriptor d{};
        d.layout = pipeline.GetBindGroupLayout(index);
        d.entryCount = entries.size();
        d.entries = entries.data();
        return device.CreateBindGroup(&d);
    };
    const auto entry = [](u32 binding, const wgpu::Buffer& buffer) {
        wgpu::BindGroupEntry e{};
        e.binding = binding;
        e.buffer = buffer;
        return e;
    };
    const wgpu::BindGroup scene = group(0, {entry(0, frameBuffer), entry(1, styleBuffer)});
    const wgpu::BindGroup output = group(1, {entry(0, out)});
    const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    const wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, scene);
    pass.SetBindGroup(1, output);
    pass.DispatchWorkgroups(1);
    pass.End();
    encoder.CopyBufferToBuffer(out, 0, readback, 0, 68);
    const wgpu::CommandBuffer commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    dawn.instance.WaitAny(readback.MapAsync(wgpu::MapMode::Read, 0, 68,
                                            wgpu::CallbackMode::WaitAnyOnly,
                                            [](wgpu::MapAsyncStatus, wgpu::StringView) {}),
                          UINT64_MAX);
    const auto* fields = static_cast<const float*>(readback.GetConstMappedRange(0, 68));
    for (int i = 0; i < 17; ++i) {
        CAPTURE(i);
        CHECK(fields[i] == doctest::Approx(expected[i]));
    }
    readback.Unmap();
}

TEST_CASE("the WebGPU renderer draws every style as Metal does") {
    @autoreleasepool {
        const std::string blob = test::importDrive(GEO_TEST_WORKER_PATH);
        Dawn dawn = makeDawn();
        std::string error;
        std::unique_ptr<webgpu::Renderer> web = webgpu::Renderer::create(
            dawn.instance, dawn.device, wgpu::TextureFormat::BGRA8Unorm, error);
        REQUIRE_MESSAGE(web != nullptr, error);
        std::unique_ptr<gpu::Renderer> metal =
            gpu::Renderer::create(MTLCreateSystemDefaultDevice(), GEO_TEST_SHADER_LIBRARY);
        REQUIRE(metal != nullptr);
        const char* keep = std::getenv("GEOMETRY_WEBGPU_IMAGES");

        const char* styles[] = {"illustration", "metal", "clay", "blueprint", "technical"};
        const char* modes[] = {"material", "parts", "mono", "file"};
        for (u32 style = 0; style < 5; ++style) {
            const u32 modeCount = style >= 3 ? 1 : 4;
            for (u32 mode = 0; mode < modeCount; ++mode) {
                const std::string name = std::format("{}-{}", styles[style], modes[mode]);
                CAPTURE(name);
                GeoRenderOptions options = geo_default_render_options();
                options.width = kWidth;
                options.height = kHeight;
                options.overlay = false;
                options.look.style = GeoStyle(style);
                options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
                options.look.color_mode = GeoColorMode(mode);
                const std::vector<u8> expected = renderWith(*metal, blob, options);
                const std::vector<u8> image = renderWith(*web, blob, options);
                REQUIRE(image.size() == expected.size());
                if (keep != nullptr) {
                    engine::writePng(std::format("{}/{}-metal.png", keep, name).c_str(), expected,
                                     kWidth, kHeight);
                    engine::writePng(std::format("{}/{}-webgpu.png", keep, name).c_str(), image,
                                     kWidth, kHeight);
                }
                const Difference d = compare(image, expected);
                CAPTURE(d.mean);
                CAPTURE(d.changed);
                CHECK(d.mean < 1.0);
                CHECK(d.changed < 0.005);
            }
        }
        unlink(blob.c_str());
    }
}

#endif
#endif // GEO_HAS_WEBGPU
