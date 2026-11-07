#include "renderer.h"

#include "shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace geo::spike {
namespace {

// 224 bytes, as the shaders' Frame.
struct FrameUniforms {
    float viewProjection[16]; // by columns
    float viewRotation[16];
    float eye[3];
    u32 perspective;
    float right[3];
    float halfWidth;
    float up[3];
    float halfHeight;
    float forward[3];
    float tanHalfX;
    float viewport[2];
    float tanHalfY;
    float secHalfX;
    float secHalfY;
    u32 instanceCount;
    u32 openClusterStart;
    u32 pad0;
};
static_assert(sizeof(FrameUniforms) == 224);

constexpr u32 kQueries = 6;
constexpr u64 kReadbackBytes = kQueries * 8 + 32; // timestamps, then both draws' arguments

using Vec3 = std::array<float, 3>;

Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}

wgpu::Buffer makeBuffer(const wgpu::Device& device, u64 size, wgpu::BufferUsage usage,
                        const char* label) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.label = label;
    descriptor.size = std::max<u64>((size + 3) & ~u64(3), 16);
    descriptor.usage = usage;
    return device.CreateBuffer(&descriptor);
}

wgpu::BindGroupEntry bufferEntry(u32 binding, const wgpu::Buffer& buffer) {
    wgpu::BindGroupEntry entry{};
    entry.binding = binding;
    entry.buffer = buffer;
    return entry;
}

} // namespace

bool Renderer::init(wgpu::Device device, const SceneData& scene, wgpu::TextureFormat outputFormat,
                    wgpu::CallbackMode callbackMode, std::string& error) {
    device_ = device;
    queue_ = device.GetQueue();
    callbackMode_ = callbackMode;
    outputFormat_ = outputFormat;
    instanceCount_ = u32(scene.instances.size());
    openClusterStart_ = u32(scene.closedClusters);
    for (u32 axis = 0; axis < 3; ++axis) {
        center_[axis] = 0.5f * (scene.boundsMin[axis] + scene.boundsMax[axis]);
    }
    const Vec3 extent = {scene.boundsMax[0] - scene.boundsMin[0],
                         scene.boundsMax[1] - scene.boundsMin[1],
                         scene.boundsMax[2] - scene.boundsMin[2]};
    radius_ = std::max(0.5f * std::sqrt(dot(extent, extent)), 1e-6f);

    wgpu::Limits limits{};
    device.GetLimits(&limits);
    for (u32 s = 0; s < kStreams; ++s) {
        if (scene.streamBytes[s] > limits.maxStorageBufferBindingSize) {
            error = "A geometry stream is larger than the device can bind.";
            return false;
        }
    }

    using U = wgpu::BufferUsage;
    const U input = U::Storage | U::CopyDst;
    frameBuffer_ = makeBuffer(device, sizeof(FrameUniforms), U::Uniform | U::CopyDst, "frame");
    definitions_ = makeBuffer(device, scene.definitions.size() * sizeof(GpuDefinition), input,
                              "definitions");
    occurrences_ = makeBuffer(device, scene.occurrences.size() * sizeof(GpuOccurrence), input,
                              "occurrences");
    instances_ = makeBuffer(device, scene.instances.size() * 8, input, "instances");
    clusters_ = makeBuffer(device, (scene.closedClusters + scene.openClusters) * 4, U::Storage,
                           "clusters");
    args_ = makeBuffer(device, 32, U::Storage | U::Indirect | U::CopyDst | U::CopySrc, "args");
    const char* names[kStreams] = {"meshlets", "vertices", "highBits", "triangles"};
    for (u32 s = 0; s < kStreams; ++s) {
        streams_[s] = makeBuffer(device, scene.streamBytes[s], input, names[s]);
    }
    queue_.WriteBuffer(definitions_, 0, scene.definitions.data(),
                       scene.definitions.size() * sizeof(GpuDefinition));
    queue_.WriteBuffer(occurrences_, 0, scene.occurrences.data(),
                       scene.occurrences.size() * sizeof(GpuOccurrence));
    queue_.WriteBuffer(instances_, 0, scene.instances.data(), scene.instances.size() * 8);
    for (const Piece& piece : scene.pieces) {
        // Pieces are whole records, so their sizes are multiples of four bytes.
        queue_.WriteBuffer(streams_[piece.stream], piece.offset, piece.data, usize(piece.size));
    }

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShaderSource;
    wgpu::ShaderModuleDescriptor moduleDescriptor{};
    moduleDescriptor.nextInChain = &wgsl;
    const wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

    wgpu::ComputePipelineDescriptor cull{};
    cull.label = "cull";
    cull.compute.module = module;
    cull.compute.entryPoint = "cull";
    cull_ = device.CreateComputePipeline(&cull);

    // Reversed depth: near is 1, compared with greater, for the most precision far away.
    wgpu::DepthStencilState depth{};
    depth.format = wgpu::TextureFormat::Depth32Float;
    depth.depthWriteEnabled = wgpu::OptionalBool::True;
    depth.depthCompare = wgpu::CompareFunction::Greater;
    wgpu::ColorTargetState visTarget{};
    visTarget.format = wgpu::TextureFormat::R32Uint;
    wgpu::FragmentState visFragment{};
    visFragment.module = module;
    visFragment.entryPoint = "visFragment";
    visFragment.targetCount = 1;
    visFragment.targets = &visTarget;
    wgpu::RenderPipelineDescriptor vis{};
    vis.vertex.module = module;
    vis.vertex.entryPoint = "visVertex";
    vis.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    vis.primitive.frontFace = wgpu::FrontFace::CCW;
    vis.depthStencil = &depth;
    vis.fragment = &visFragment;
    vis.label = "vis closed";
    vis.primitive.cullMode = wgpu::CullMode::Back;
    visClosed_ = device.CreateRenderPipeline(&vis);
    vis.label = "vis open";
    vis.primitive.cullMode = wgpu::CullMode::None;
    visOpen_ = device.CreateRenderPipeline(&vis);

    wgpu::ColorTargetState output{};
    output.format = outputFormat;
    wgpu::FragmentState compositeFragment{};
    compositeFragment.module = module;
    compositeFragment.entryPoint = "composite";
    compositeFragment.targetCount = 1;
    compositeFragment.targets = &output;
    wgpu::RenderPipelineDescriptor composite{};
    composite.label = "composite";
    composite.vertex.module = module;
    composite.vertex.entryPoint = "fullscreen";
    composite.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    composite.fragment = &compositeFragment;
    composite_ = device.CreateRenderPipeline(&composite);

    if (device.HasFeature(wgpu::FeatureName::TimestampQuery)) {
        wgpu::QuerySetDescriptor queries{};
        queries.type = wgpu::QueryType::Timestamp;
        queries.count = kQueries;
        querySet_ = device.CreateQuerySet(&queries);
        resolve_ = makeBuffer(device, kQueries * 8, U::QueryResolve | U::CopySrc, "resolve");
    }
    for (Readback& readback : readbacks_) {
        readback.buffer = makeBuffer(device, kReadbackBytes, U::MapRead | U::CopyDst, "readback");
    }
    return true;
}

void Renderer::resize(u32 width, u32 height) {
    if (width == width_ && height == height_) {
        return;
    }
    width_ = width;
    height_ = height;
    createTargets();
    createBindGroups();
}

void Renderer::createTargets() {
    wgpu::TextureDescriptor descriptor{};
    descriptor.size = {width_, height_, 1};
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    descriptor.format = wgpu::TextureFormat::R32Uint;
    descriptor.label = "vis";
    vis_ = device_.CreateTexture(&descriptor);
    descriptor.format = wgpu::TextureFormat::Depth32Float;
    descriptor.label = "depth";
    depth_ = device_.CreateTexture(&descriptor);
    visView_ = vis_.CreateView();
    depthView_ = depth_.CreateView();
}

void Renderer::createBindGroups() {
    const wgpu::Buffer& meshlets = streams_[kStreamMeshlets];
    const wgpu::Buffer& vertices = streams_[kStreamVertices];
    const wgpu::Buffer& highBits = streams_[kStreamHighBits];
    const wgpu::Buffer& triangles = streams_[kStreamTriangles];

    const wgpu::BindGroupEntry cull[] = {
        bufferEntry(0, frameBuffer_), bufferEntry(1, definitions_), bufferEntry(2, occurrences_),
        bufferEntry(3, meshlets),     bufferEntry(4, instances_),   bufferEntry(9, clusters_),
        bufferEntry(10, args_),
    };
    wgpu::BindGroupDescriptor group{};
    group.layout = cull_.GetBindGroupLayout(0);
    group.entryCount = std::size(cull);
    group.entries = cull;
    cullGroup_ = device_.CreateBindGroup(&group);

    const wgpu::BindGroupEntry vis[] = {
        bufferEntry(0, frameBuffer_), bufferEntry(1, definitions_), bufferEntry(2, occurrences_),
        bufferEntry(3, meshlets),     bufferEntry(4, instances_),   bufferEntry(5, vertices),
        bufferEntry(6, highBits),     bufferEntry(7, triangles),    bufferEntry(8, clusters_),
    };
    group.entryCount = std::size(vis);
    group.entries = vis;
    group.layout = visClosed_.GetBindGroupLayout(0);
    visClosedGroup_ = device_.CreateBindGroup(&group);
    group.layout = visOpen_.GetBindGroupLayout(0);
    visOpenGroup_ = device_.CreateBindGroup(&group);

    wgpu::BindGroupEntry composite[10];
    std::copy(std::begin(vis), std::end(vis), composite);
    composite[9] = {};
    composite[9].binding = 11;
    composite[9].textureView = visView_;
    group.entryCount = 10;
    group.entries = composite;
    group.layout = composite_.GetBindGroupLayout(0);
    compositeGroup_ = device_.CreateBindGroup(&group);
}

void Renderer::frame(const View& view, wgpu::TextureView target) {
    // An orthographic camera on a sphere around the model, z up.
    const float az = view.azimuth * float(M_PI) / 180.f;
    const float el = view.elevation * float(M_PI) / 180.f;
    const Vec3 forward = {-std::cos(el) * std::cos(az), -std::cos(el) * std::sin(az), -std::sin(el)};
    const Vec3 right = normalize(cross(forward, {0.f, 0.f, 1.f}));
    const Vec3 up = cross(right, forward);
    const Vec3 eye = {center_[0] - forward[0] * 2.f * radius_,
                      center_[1] - forward[1] * 2.f * radius_,
                      center_[2] - forward[2] * 2.f * radius_};
    const float aspect = float(width_) / float(height_);
    const float halfHeight = radius_ / view.zoom * (aspect < 1.f ? 1.f / aspect : 1.f);
    const float halfWidth = halfHeight * aspect;
    const float near = radius_, far = 3.f * radius_;

    FrameUniforms u{};
    // Rows of the view-projection, stored by columns: x and y scaled to the half extents, and
    // depth reversed so the near plane is 1.
    const float rows[4][4] = {
        {right[0] / halfWidth, right[1] / halfWidth, right[2] / halfWidth, -dot(right, eye) / halfWidth},
        {up[0] / halfHeight, up[1] / halfHeight, up[2] / halfHeight, -dot(up, eye) / halfHeight},
        {-forward[0] / (far - near), -forward[1] / (far - near), -forward[2] / (far - near),
         (far + dot(forward, eye)) / (far - near)},
        {0.f, 0.f, 0.f, 1.f},
    };
    // World to view: right, up and towards the eye.
    const float rotation[4][4] = {
        {right[0], right[1], right[2], 0.f},
        {up[0], up[1], up[2], 0.f},
        {-forward[0], -forward[1], -forward[2], 0.f},
        {0.f, 0.f, 0.f, 1.f},
    };
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            u.viewProjection[c * 4 + r] = rows[r][c];
            u.viewRotation[c * 4 + r] = rotation[r][c];
        }
    }
    std::copy_n(eye.data(), 3, u.eye);
    std::copy_n(right.data(), 3, u.right);
    std::copy_n(up.data(), 3, u.up);
    std::copy_n(forward.data(), 3, u.forward);
    u.halfWidth = halfWidth;
    u.halfHeight = halfHeight;
    u.viewport[0] = float(width_);
    u.viewport[1] = float(height_);
    u.instanceCount = instanceCount_;
    u.openClusterStart = openClusterStart_;
    queue_.WriteBuffer(frameBuffer_, 0, &u, sizeof(u));
    const u32 args[8] = {0, 1, 0, 0, 0, 1, openClusterStart_ * 48, 0};
    queue_.WriteBuffer(args_, 0, args, sizeof(args));

    const wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
    wgpu::PassTimestampWrites writes[3];
    for (u32 pass = 0; pass < 3; ++pass) {
        writes[pass].querySet = querySet_;
        writes[pass].beginningOfPassWriteIndex = pass * 2;
        writes[pass].endOfPassWriteIndex = pass * 2 + 1;
    }

    wgpu::ComputePassDescriptor cullPass{};
    cullPass.timestampWrites = querySet_ ? &writes[0] : nullptr;
    const wgpu::ComputePassEncoder compute = encoder.BeginComputePass(&cullPass);
    compute.SetPipeline(cull_);
    compute.SetBindGroup(0, cullGroup_);
    const u32 groups = (instanceCount_ + 63) / 64;
    const u32 x = std::min(groups, 65535u);
    compute.DispatchWorkgroups(x, (groups + x - 1) / std::max(x, 1u));
    compute.End();

    wgpu::RenderPassColorAttachment visColor{};
    visColor.view = visView_;
    visColor.loadOp = wgpu::LoadOp::Clear;
    visColor.storeOp = wgpu::StoreOp::Store;
    visColor.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDepthStencilAttachment visDepth{};
    visDepth.view = depthView_;
    visDepth.depthLoadOp = wgpu::LoadOp::Clear;
    visDepth.depthStoreOp = wgpu::StoreOp::Store;
    visDepth.depthClearValue = 0.f;
    wgpu::RenderPassDescriptor visPass{};
    visPass.colorAttachmentCount = 1;
    visPass.colorAttachments = &visColor;
    visPass.depthStencilAttachment = &visDepth;
    visPass.timestampWrites = querySet_ ? &writes[1] : nullptr;
    const wgpu::RenderPassEncoder visEncoder = encoder.BeginRenderPass(&visPass);
    visEncoder.SetPipeline(visClosed_);
    visEncoder.SetBindGroup(0, visClosedGroup_);
    visEncoder.DrawIndirect(args_, 0);
    visEncoder.SetPipeline(visOpen_);
    visEncoder.SetBindGroup(0, visOpenGroup_);
    visEncoder.DrawIndirect(args_, 16);
    visEncoder.End();

    wgpu::RenderPassColorAttachment color{};
    color.view = target;
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor compositePass{};
    compositePass.colorAttachmentCount = 1;
    compositePass.colorAttachments = &color;
    compositePass.timestampWrites = querySet_ ? &writes[2] : nullptr;
    const wgpu::RenderPassEncoder compositeEncoder = encoder.BeginRenderPass(&compositePass);
    compositeEncoder.SetPipeline(composite_);
    compositeEncoder.SetBindGroup(0, compositeGroup_);
    compositeEncoder.Draw(3);
    compositeEncoder.End();

    // Read the timings back through a free buffer of the ring; a frame with none free goes untimed.
    Readback* readback = nullptr;
    for (Readback& candidate : readbacks_) {
        if (!candidate.busy) {
            readback = &candidate;
            break;
        }
    }
    if (readback != nullptr) {
        if (querySet_) {
            encoder.ResolveQuerySet(querySet_, 0, kQueries, resolve_, 0);
            encoder.CopyBufferToBuffer(resolve_, 0, readback->buffer, 0, kQueries * 8);
        }
        encoder.CopyBufferToBuffer(args_, 0, readback->buffer, kQueries * 8, 32);
    }
    const wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);

    if (readback != nullptr) {
        readback->busy = true;
        readback->buffer.MapAsync(
            wgpu::MapMode::Read, 0, kReadbackBytes, callbackMode_,
            [this, readback, tag = tag_](wgpu::MapAsyncStatus status, wgpu::StringView) {
                if (status == wgpu::MapAsyncStatus::Success) {
                    const auto* bytes =
                        static_cast<const u8*>(readback->buffer.GetConstMappedRange(0, kReadbackBytes));
                    u64 t[kQueries];
                    u32 args[8];
                    std::memcpy(t, bytes, sizeof(t));
                    std::memcpy(args, bytes + sizeof(t), sizeof(args));
                    FrameTiming timing;
                    timing.tag = tag;
                    if (querySet_) {
                        timing.cull = double(t[1] - t[0]) * 1e-6;
                        timing.vis = double(t[3] - t[2]) * 1e-6;
                        timing.composite = double(t[5] - t[4]) * 1e-6;
                        timing.total = double(t[5] - t[0]) * 1e-6;
                    }
                    timing.clusters = (u64(args[0]) + u64(args[4])) / 48;
                    if (std::getenv("SPIKE_RAW_TIMESTAMPS") != nullptr) {
                        std::fprintf(stderr, "%llu %llu %llu %llu %llu %llu\n", t[0], t[1], t[2],
                                     t[3], t[4], t[5]);
                    }
                    timings_.push_back(timing);
                    readback->buffer.Unmap();
                }
                readback->busy = false;
            });
    }
}

} // namespace geo::spike
