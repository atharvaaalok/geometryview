#include "renderer.h"

#include "shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace geo::spike {
namespace {

// 272 bytes, as the shaders' Frame.
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
    u32 edgeInstanceCount;
    float lightTravel[3];
    float ground;
    float worldPerPixel;
    float pixelsPerPoint;
    float depthNear;
    float depthFar;
    float aoRadius;
    u32 aoSamples;
    u32 features;
    u32 openIndexStart;
};
static_assert(sizeof(FrameUniforms) == 272);

enum Pass : u32 { kPassCull, kPassShadow, kPassVis, kPassOcclusion, kPassComposite, kPasses };
constexpr u32 kQueries = kPasses * 2;
constexpr u64 kReadbackBytes = kQueries * 8 + 40; // timestamps, then the surface draws' arguments

using Vec3 = std::array<float, 3>;

Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}

wgpu::BindGroupEntry bufferEntry(u32 binding, const wgpu::Buffer& buffer) {
    wgpu::BindGroupEntry entry{};
    entry.binding = binding;
    entry.buffer = buffer;
    return entry;
}

wgpu::BindGroupEntry textureEntry(u32 binding, const wgpu::TextureView& view) {
    wgpu::BindGroupEntry entry{};
    entry.binding = binding;
    entry.textureView = view;
    return entry;
}

// Groups for dispatching `count` threads in workgroups of 64, two-dimensional past the limit.
std::array<u32, 2> dispatchSize(u32 count) {
    const u32 groups = (count + 63) / 64;
    const u32 x = std::min(groups, 65535u);
    return {x, x == 0 ? 0 : (groups + x - 1) / x};
}

} // namespace

wgpu::Buffer Renderer::buffer(u64 size, wgpu::BufferUsage usage, const char* label) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.label = label;
    descriptor.size = std::max<u64>((size + 3) & ~u64(3), 16);
    descriptor.usage = usage;
    bufferBytes_ += descriptor.size;
    return device_.CreateBuffer(&descriptor);
}

wgpu::Texture Renderer::texture(u32 width, u32 height, wgpu::TextureFormat format,
                                const char* label, wgpu::TextureUsage extra) {
    wgpu::TextureDescriptor descriptor{};
    descriptor.label = label;
    descriptor.size = {std::max(width, 1u), std::max(height, 1u), 1};
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.format = format;
    descriptor.usage =
        wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | extra;
    u64 texel = 4;
    switch (format) {
    case wgpu::TextureFormat::R8Unorm:
    case wgpu::TextureFormat::R8Uint:
        texel = 1;
        break;
    case wgpu::TextureFormat::RG32Uint:
        texel = 8;
        break;
    default:
        break;
    }
    targetBytes_ += u64(descriptor.size.width) * descriptor.size.height * texel;
    return device_.CreateTexture(&descriptor);
}

bool Renderer::init(wgpu::Device device, const SceneData& scene, Mode mode, Features features,
                    float pixelsPerPoint, wgpu::TextureFormat outputFormat,
                    wgpu::CallbackMode callbackMode, std::string& error) {
    device_ = device;
    queue_ = device.GetQueue();
    mode_ = mode;
    features_ = features;
    pixelsPerPoint_ = pixelsPerPoint;
    callbackMode_ = callbackMode;
    outputFormat_ = outputFormat;
    instanceCount_ = u32(scene.instances.size());
    edgeInstanceCount_ = u32(scene.edgeInstances.size());
    const bool indexed = mode == Mode::Indexed;
    if (!indexed) {
        features_ = {false, false, false}; // the pulled mode draws surfaces only
    }
    openStart_ = u32(indexed ? scene.closedInstances : scene.closedClusters);
    boundsMin_ = scene.boundsMin;
    for (u32 axis = 0; axis < 3; ++axis) {
        center_[axis] = 0.5f * (scene.boundsMin[axis] + scene.boundsMax[axis]);
    }
    const Vec3 extent = {scene.boundsMax[0] - scene.boundsMin[0],
                         scene.boundsMax[1] - scene.boundsMin[1],
                         scene.boundsMax[2] - scene.boundsMin[2]};
    radius_ = std::max(0.5f * std::sqrt(dot(extent, extent)), 1e-6f);

    wgpu::Limits limits{};
    device.GetLimits(&limits);
    openIndexStart_ = u32(scene.closedIndices);
    const u64 indexBytes = (scene.closedIndices + scene.openIndices) * 4;
    for (u32 s = 0; s < kStreams; ++s) {
        if (scene.streamBytes[s] > limits.maxStorageBufferBindingSize) {
            error = "A geometry stream is larger than the device can bind.";
            return false;
        }
    }
    if (indexed && indexBytes > limits.maxStorageBufferBindingSize) {
        error = "The indexed mode's index buffer is larger than the device can bind.";
        return false;
    }

    using U = wgpu::BufferUsage;
    const U input = U::Storage | U::CopyDst;
    const U arguments = U::Storage | U::Indirect | U::CopyDst | U::CopySrc;
    frameBuffer_ = buffer(sizeof(FrameUniforms), U::Uniform | U::CopyDst, "frame");
    definitions_ = buffer(scene.definitions.size() * sizeof(GpuDefinition), input, "definitions");
    occurrences_ = buffer(scene.occurrences.size() * sizeof(GpuOccurrence), input, "occurrences");
    instances_ = buffer(scene.instances.size() * 8, input, "instances");
    const char* names[kStreams] = {"meshlets", "vertices", "highBits", "triangles",
                                   "edgeChunks", "edgeSegments"};
    for (u32 s = 0; s < kStreams; ++s) {
        streams_[s] = buffer(scene.streamBytes[s], input, names[s]);
    }
    if (indexed) {
        slots_ = buffer(u64(instanceCount_) * 4, U::Storage, "slots");
        indices_ = buffer(indexBytes, U::Storage | U::Index, "indices");
        indexedArgs_ = buffer(48, arguments, "indexed args"); // two draws, then two slot counters
        if (features_.shadow) {
            shadowSlots_ = buffer(u64(instanceCount_) * 4, U::Storage, "shadow slots");
            shadowIndices_ = buffer(indexBytes, U::Storage | U::Index, "shadow indices");
            shadowArgs_ = buffer(48, arguments, "shadow args"); // laid out as the camera's
        }
        if (features_.edges) {
            edgeInstances_ = buffer(u64(edgeInstanceCount_) * 8, input, "edge instances");
            edgeSlots_ = buffer(u64(edgeInstanceCount_) * 4, U::Storage, "edge slots");
            edgeArgs_ = buffer(16, arguments, "edge args");
            queue_.WriteBuffer(edgeInstances_, 0, scene.edgeInstances.data(),
                               scene.edgeInstances.size() * 8);
        }
    } else {
        clusters_ = buffer((scene.closedClusters + scene.openClusters) * 4, U::Storage, "clusters");
        args_ = buffer(32, arguments, "args");
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
    module_ = device.CreateShaderModule(&moduleDescriptor);
    createPipelines();

    if (device.HasFeature(wgpu::FeatureName::TimestampQuery)) {
        wgpu::QuerySetDescriptor queries{};
        queries.type = wgpu::QueryType::Timestamp;
        queries.count = kQueries;
        querySet_ = device.CreateQuerySet(&queries);
        resolve_ = buffer(kQueries * 8, U::QueryResolve | U::CopySrc, "resolve");
    }
    for (Readback& readback : readbacks_) {
        readback.buffer = buffer(kReadbackBytes, U::MapRead | U::CopyDst, "readback");
    }
    for (Readback& readback : pickReadbacks_) {
        readback.buffer = buffer(256, U::MapRead | U::CopyDst, "pick");
    }
    return true;
}

void Renderer::createPipelines() {
    const bool indexed = mode_ == Mode::Indexed;
    static const wgpu::ConstantEntry kShadowOn = [] {
        wgpu::ConstantEntry entry{};
        entry.key = "kShadow";
        entry.value = 1.0;
        return entry;
    }();

    auto compute = [&](const char* entry, bool shadow) {
        wgpu::ComputePipelineDescriptor descriptor{};
        descriptor.label = entry;
        descriptor.compute.module = module_;
        descriptor.compute.entryPoint = entry;
        descriptor.compute.constantCount = shadow ? 1 : 0;
        descriptor.compute.constants = shadow ? &kShadowOn : nullptr;
        return device_.CreateComputePipeline(&descriptor);
    };
    cull_ = compute(indexed ? "cullIndexed" : "cull", false);
    if (features_.shadow) {
        cullShadow_ = compute("cullIndexed", true);
    }
    if (features_.edges) {
        cullEdges_ = compute("cullEdges", false);
    }

    // Reversed depth: near is 1, compared with greater, for the most precision far away.
    wgpu::DepthStencilState depthWrite{};
    depthWrite.format = wgpu::TextureFormat::Depth32Float;
    depthWrite.depthWriteEnabled = wgpu::OptionalBool::True;
    depthWrite.depthCompare = wgpu::CompareFunction::Greater;
    wgpu::DepthStencilState depthTest = depthWrite;
    depthTest.depthWriteEnabled = wgpu::OptionalBool::False;

    // The visibility pass's targets: ids, then in the indexed mode the normal and the edges' ink,
    // which keeps the larger coverage where quads overlap.
    wgpu::BlendState keepLarger{};
    keepLarger.color = {wgpu::BlendOperation::Max, wgpu::BlendFactor::One, wgpu::BlendFactor::One};
    keepLarger.alpha = keepLarger.color;
    wgpu::ColorTargetState visTargets[4] = {};
    visTargets[0].format = indexed ? wgpu::TextureFormat::RG32Uint : wgpu::TextureFormat::R32Uint;
    visTargets[1].format = wgpu::TextureFormat::RG16Float;
    visTargets[2].format = wgpu::TextureFormat::R8Unorm;
    visTargets[2].writeMask = wgpu::ColorWriteMask::None;
    visTargets[2].blend = &keepLarger;
    visTargets[3].format = wgpu::TextureFormat::R8Uint;

    wgpu::FragmentState visFragment{};
    visFragment.module = module_;
    visFragment.entryPoint = indexed ? "visIndexedFragment" : "visFragment";
    visFragment.targetCount = indexed ? 4 : 1;
    visFragment.targets = visTargets;
    wgpu::RenderPipelineDescriptor vis{};
    vis.vertex.module = module_;
    vis.vertex.entryPoint = indexed ? "visIndexedVertex" : "visVertex";
    vis.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    vis.primitive.frontFace = wgpu::FrontFace::CCW;
    vis.depthStencil = &depthWrite;
    vis.fragment = &visFragment;
    vis.label = "vis closed";
    vis.primitive.cullMode = wgpu::CullMode::Back;
    visClosed_ = device_.CreateRenderPipeline(&vis);
    vis.label = "vis open";
    vis.primitive.cullMode = wgpu::CullMode::None;
    visOpen_ = device_.CreateRenderPipeline(&vis);

    if (features_.edges) {
        wgpu::ColorTargetState edgeTargets[4] = {visTargets[0], visTargets[1], visTargets[2],
                                                 visTargets[3]};
        edgeTargets[0].writeMask = wgpu::ColorWriteMask::None;
        edgeTargets[1].writeMask = wgpu::ColorWriteMask::None;
        edgeTargets[2].writeMask = wgpu::ColorWriteMask::Red;
        edgeTargets[3].writeMask = wgpu::ColorWriteMask::None;
        wgpu::FragmentState fragment = visFragment;
        fragment.entryPoint = "edgeFragment";
        fragment.targets = edgeTargets;
        wgpu::RenderPipelineDescriptor edges = vis;
        edges.label = "edges";
        edges.vertex.entryPoint = "edgeVertex";
        edges.depthStencil = &depthTest;
        edges.fragment = &fragment;
        edges_ = device_.CreateRenderPipeline(&edges);
    }

    wgpu::ColorTargetState mask{};
    mask.format = wgpu::TextureFormat::R8Unorm;
    wgpu::FragmentState maskFragment{};
    maskFragment.module = module_;
    maskFragment.targetCount = 1;
    maskFragment.targets = &mask;
    if (features_.shadow) {
        maskFragment.entryPoint = "shadowFragment";
        wgpu::RenderPipelineDescriptor shadow{};
        shadow.label = "shadow";
        shadow.vertex.module = module_;
        shadow.vertex.entryPoint = "visIndexedVertex";
        shadow.vertex.constantCount = 1;
        shadow.vertex.constants = &kShadowOn;
        shadow.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        shadow.primitive.cullMode = wgpu::CullMode::None;
        shadow.fragment = &maskFragment;
        shadow_ = device_.CreateRenderPipeline(&shadow);
    }

    wgpu::RenderPipelineDescriptor fullscreen{};
    fullscreen.vertex.module = module_;
    fullscreen.vertex.entryPoint = "fullscreen";
    fullscreen.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    if (features_.occlusion) {
        mask.format = wgpu::TextureFormat::RG16Float; // occlusion, and the view depth it was at
        maskFragment.entryPoint = "occlusion";
        fullscreen.label = "occlusion";
        fullscreen.fragment = &maskFragment;
        occlusion_ = device_.CreateRenderPipeline(&fullscreen);
    }
    wgpu::ColorTargetState output{};
    output.format = outputFormat_;
    wgpu::FragmentState compositeFragment{};
    compositeFragment.module = module_;
    compositeFragment.entryPoint = indexed ? "compositeIndexed" : "composite";
    compositeFragment.targetCount = 1;
    compositeFragment.targets = &output;
    fullscreen.label = "composite";
    fullscreen.fragment = &compositeFragment;
    composite_ = device_.CreateRenderPipeline(&fullscreen);
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
    using F = wgpu::TextureFormat;
    targetBytes_ = 0;
    const bool indexed = mode_ == Mode::Indexed;
    const u32 halfWidth = (width_ + 1) / 2, halfHeight = (height_ + 1) / 2;
    vis_ = texture(width_, height_, indexed ? F::RG32Uint : F::R32Uint, "vis",
                   wgpu::TextureUsage::CopySrc); // the hover probe copies from it
    visView_ = vis_.CreateView();
    depth_ = texture(width_, height_, F::Depth32Float, "depth");
    depthView_ = depth_.CreateView();
    if (indexed) {
        normal_ = texture(width_, height_, F::RG16Float, "normal");
        normalView_ = normal_.CreateView();
        // The composite reads these whether or not they are drawn, so they always exist.
        ink_ = texture(width_, height_, F::R8Unorm, "ink");
        inkView_ = ink_.CreateView();
        tag_ = texture(width_, height_, F::R8Uint, "tag");
        tagView_ = tag_.CreateView();
        occlusionTarget_ = texture(halfWidth, halfHeight, F::RG16Float, "occlusion");
        occlusionView_ = occlusionTarget_.CreateView();
        shadowMask_ = texture(halfWidth, halfHeight, F::R8Unorm, "shadow");
        shadowView_ = shadowMask_.CreateView();
    }
}

template <typename Pipeline>
wgpu::BindGroup Renderer::group(const Pipeline& pipeline,
                                std::initializer_list<wgpu::BindGroupEntry> entries) {
    wgpu::BindGroupDescriptor descriptor{};
    descriptor.layout = pipeline.GetBindGroupLayout(0);
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.begin();
    return device_.CreateBindGroup(&descriptor);
}

void Renderer::createBindGroups() {
    const wgpu::Buffer& meshlets = streams_[kStreamMeshlets];
    const wgpu::Buffer& vertices = streams_[kStreamVertices];
    const wgpu::Buffer& highBits = streams_[kStreamHighBits];
    const wgpu::Buffer& triangles = streams_[kStreamTriangles];
    const auto b = bufferEntry;
    const auto t = textureEntry;

    if (mode_ == Mode::Pulled) {
        cullGroup_ = group(cull_, {b(0, frameBuffer_), b(1, definitions_), b(2, occurrences_),
                                   b(3, meshlets), b(4, instances_), b(9, clusters_), b(10, args_)});
        visClosedGroup_ = group(visClosed_, {b(0, frameBuffer_), b(1, definitions_),
                                             b(2, occurrences_), b(3, meshlets), b(4, instances_),
                                             b(5, vertices), b(6, highBits), b(7, triangles),
                                             b(8, clusters_)});
        visOpenGroup_ = group(visOpen_, {b(0, frameBuffer_), b(1, definitions_),
                                         b(2, occurrences_), b(3, meshlets), b(4, instances_),
                                         b(5, vertices), b(6, highBits), b(7, triangles),
                                         b(8, clusters_)});
        compositeGroup_ = group(composite_, {b(0, frameBuffer_), b(1, definitions_),
                                             b(2, occurrences_), b(3, meshlets), b(4, instances_),
                                             b(5, vertices), b(6, highBits), b(7, triangles),
                                             b(8, clusters_), t(11, visView_)});
        return;
    }

    cullGroup_ = group(cull_, {b(0, frameBuffer_), b(1, definitions_), b(2, occurrences_),
                               b(3, meshlets), b(4, instances_), b(7, triangles), b(12, slots_),
                               b(13, indices_), b(14, indexedArgs_)});
    visClosedGroup_ = group(visClosed_, {b(0, frameBuffer_), b(1, definitions_),
                                         b(2, occurrences_), b(3, meshlets), b(4, instances_),
                                         b(5, vertices), b(6, highBits), b(15, slots_)});
    visOpenGroup_ = group(visOpen_, {b(0, frameBuffer_), b(1, definitions_), b(2, occurrences_),
                                     b(3, meshlets), b(4, instances_), b(5, vertices),
                                     b(6, highBits), b(15, slots_)});
    if (features_.shadow) {
        cullShadowGroup_ = group(cullShadow_, {b(0, frameBuffer_), b(1, definitions_),
                                               b(2, occurrences_), b(3, meshlets),
                                               b(4, instances_), b(7, triangles),
                                               b(12, shadowSlots_), b(13, shadowIndices_),
                                               b(14, shadowArgs_)});
        shadowGroup_ = group(shadow_, {b(0, frameBuffer_), b(1, definitions_), b(2, occurrences_),
                                       b(3, meshlets), b(4, instances_), b(5, vertices),
                                       b(6, highBits), b(15, shadowSlots_)});
    }
    if (features_.edges) {
        const wgpu::Buffer& chunks = streams_[kStreamEdgeChunks];
        const wgpu::Buffer& segments = streams_[kStreamEdgeSegments];
        cullEdgesGroup_ = group(cullEdges_, {b(0, frameBuffer_), b(1, definitions_),
                                             b(2, occurrences_), b(18, edgeInstances_),
                                             b(19, chunks), b(21, edgeSlots_), b(22, edgeArgs_)});
        edgesGroup_ = group(edges_, {b(0, frameBuffer_), b(1, definitions_), b(2, occurrences_),
                                     b(5, vertices), b(6, highBits), b(18, edgeInstances_),
                                     b(19, chunks), b(20, segments), b(23, edgeSlots_)});
    }
    if (features_.occlusion) {
        occlusionGroup_ = group(occlusion_, {b(0, frameBuffer_), t(28, depthView_),
                                             t(32, tagView_)});
    }
    compositeGroup_ = group(composite_, {b(0, frameBuffer_), t(16, visView_), t(17, normalView_),
                                         t(28, depthView_), t(29, inkView_),
                                         t(30, occlusionView_), t(31, shadowView_),
                                         t(32, tagView_)});
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
    u.openClusterStart = openStart_;
    u.edgeInstanceCount = edgeInstanceCount_;
    // A high sun whose bearing follows the camera, so the shadow falls toward the viewer's lower
    // left, onto a ground just below the model.
    const Vec3 toward = normalize({-forward[0] - right[0], -forward[1] - right[1], 0.f});
    const float sun = 75.f * float(M_PI) / 180.f;
    const Vec3 travel = {toward[0] * std::cos(sun), toward[1] * std::cos(sun), -std::sin(sun)};
    std::copy_n(travel.data(), 3, u.lightTravel);
    u.ground = boundsMin_[2] - 0.002f * radius_;
    u.worldPerPixel = 2.f * halfHeight / float(height_);
    u.pixelsPerPoint = pixelsPerPoint_;
    u.depthNear = near;
    u.depthFar = far;
    u.aoRadius = 0.06f * radius_;
    u.aoSamples = 12;
    u.features = (features_.edges ? 1u : 0u) | (features_.shadow ? 2u : 0u) |
                 (features_.occlusion ? 4u : 0u);
    u.openIndexStart = openIndexStart_;
    queue_.WriteBuffer(frameBuffer_, 0, &u, sizeof(u));

    const bool indexed = mode_ == Mode::Indexed;
    if (indexed) {
        // Both draws empty, the open one's indices after the closed ones', and no slots taken.
        const u32 args[12] = {0, 1, 0, 0, 0, 0, 1, openIndexStart_, 0, 0, 0, 0};
        queue_.WriteBuffer(indexedArgs_, 0, args, sizeof(args));
        if (features_.shadow) {
            queue_.WriteBuffer(shadowArgs_, 0, args, sizeof(args));
        }
        if (features_.edges) {
            const u32 edgeArgs[4] = {0, 1, 0, 0};
            queue_.WriteBuffer(edgeArgs_, 0, edgeArgs, sizeof(edgeArgs));
        }
    } else {
        const u32 args[8] = {0, 1, 0, 0, 0, 1, openStart_ * 48, 0};
        queue_.WriteBuffer(args_, 0, args, sizeof(args));
    }

    const wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
    wgpu::PassTimestampWrites writes[kPasses];
    for (u32 pass = 0; pass < kPasses; ++pass) {
        writes[pass].querySet = querySet_;
        writes[pass].beginningOfPassWriteIndex = pass * 2;
        writes[pass].endOfPassWriteIndex = pass * 2 + 1;
    }
    auto timed = [&](Pass pass) { return querySet_ ? &writes[pass] : nullptr; };

    // Cull: surfaces, and the shadow's and edges' own sets.
    wgpu::ComputePassDescriptor cullPass{};
    cullPass.timestampWrites = timed(kPassCull);
    const wgpu::ComputePassEncoder compute = encoder.BeginComputePass(&cullPass);
    auto dispatch = [&](const wgpu::ComputePipeline& pipeline, const wgpu::BindGroup& bindings,
                        u32 count) {
        const auto [x, y] = dispatchSize(count);
        compute.SetPipeline(pipeline);
        compute.SetBindGroup(0, bindings);
        compute.DispatchWorkgroups(x, y);
    };
    dispatch(cull_, cullGroup_, instanceCount_);
    if (features_.shadow) {
        dispatch(cullShadow_, cullShadowGroup_, instanceCount_);
    }
    if (features_.edges) {
        dispatch(cullEdges_, cullEdgesGroup_, edgeInstanceCount_);
    }
    compute.End();

    auto attachment = [](const wgpu::TextureView& view) {
        wgpu::RenderPassColorAttachment color{};
        color.view = view;
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = {0, 0, 0, 0};
        return color;
    };

    // The cast shadow: solid surfaces flattened onto the ground, at half resolution.
    if (features_.shadow) {
        const wgpu::RenderPassColorAttachment color = attachment(shadowView_);
        wgpu::RenderPassDescriptor pass{};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        pass.timestampWrites = timed(kPassShadow);
        const wgpu::RenderPassEncoder encoder2 = encoder.BeginRenderPass(&pass);
        encoder2.SetPipeline(shadow_);
        encoder2.SetBindGroup(0, shadowGroup_);
        encoder2.SetIndexBuffer(shadowIndices_, wgpu::IndexFormat::Uint32);
        encoder2.DrawIndexedIndirect(shadowArgs_, 0);
        encoder2.End();
    }

    // Visibility: surfaces, closed then open, then the feature edges over them.
    const wgpu::RenderPassColorAttachment visColor[4] = {
        attachment(visView_), attachment(normalView_), attachment(inkView_), attachment(tagView_)};
    wgpu::RenderPassDepthStencilAttachment visDepth{};
    visDepth.view = depthView_;
    visDepth.depthLoadOp = wgpu::LoadOp::Clear;
    visDepth.depthStoreOp = wgpu::StoreOp::Store;
    visDepth.depthClearValue = 0.f;
    wgpu::RenderPassDescriptor visPass{};
    visPass.colorAttachmentCount = indexed ? 4 : 1;
    visPass.colorAttachments = visColor;
    visPass.depthStencilAttachment = &visDepth;
    visPass.timestampWrites = timed(kPassVis);
    const wgpu::RenderPassEncoder visEncoder = encoder.BeginRenderPass(&visPass);
    visEncoder.SetPipeline(visClosed_);
    visEncoder.SetBindGroup(0, visClosedGroup_);
    if (indexed) {
        visEncoder.SetIndexBuffer(indices_, wgpu::IndexFormat::Uint32);
        visEncoder.DrawIndexedIndirect(indexedArgs_, 0);
    } else {
        visEncoder.DrawIndirect(args_, 0);
    }
    visEncoder.SetPipeline(visOpen_);
    visEncoder.SetBindGroup(0, visOpenGroup_);
    if (indexed) {
        visEncoder.DrawIndexedIndirect(indexedArgs_, 20);
    } else {
        visEncoder.DrawIndirect(args_, 16);
    }
    if (features_.edges) {
        visEncoder.SetPipeline(edges_);
        visEncoder.SetBindGroup(0, edgesGroup_);
        visEncoder.DrawIndirect(edgeArgs_, 0);
    }
    visEncoder.End();

    // Ambient occlusion from depth, at half resolution.
    if (features_.occlusion) {
        const wgpu::RenderPassColorAttachment color = attachment(occlusionView_);
        wgpu::RenderPassDescriptor pass{};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        pass.timestampWrites = timed(kPassOcclusion);
        const wgpu::RenderPassEncoder encoder2 = encoder.BeginRenderPass(&pass);
        encoder2.SetPipeline(occlusion_);
        encoder2.SetBindGroup(0, occlusionGroup_);
        encoder2.Draw(3);
        encoder2.End();
    }

    wgpu::RenderPassColorAttachment color = attachment(target);
    wgpu::RenderPassDescriptor compositePass{};
    compositePass.colorAttachmentCount = 1;
    compositePass.colorAttachments = &color;
    compositePass.timestampWrites = timed(kPassComposite);
    const wgpu::RenderPassEncoder compositeEncoder = encoder.BeginRenderPass(&compositePass);
    compositeEncoder.SetPipeline(composite_);
    compositeEncoder.SetBindGroup(0, compositeGroup_);
    compositeEncoder.Draw(3);
    compositeEncoder.End();

    // The hover probe: the ids under the cursor, copied out for the CPU to read when it can.
    Readback* pick = nullptr;
    if (indexed && view.pickX >= 0 && view.pickY >= 0 && u32(view.pickX) < width_ &&
        u32(view.pickY) < height_) {
        for (Readback& candidate : pickReadbacks_) {
            if (!candidate.busy) {
                pick = &candidate;
                break;
            }
        }
    }
    if (pick != nullptr) {
        wgpu::TexelCopyTextureInfo source{};
        source.texture = vis_;
        source.origin = {u32(view.pickX), u32(view.pickY), 0};
        wgpu::TexelCopyBufferInfo destination{};
        destination.buffer = pick->buffer;
        destination.layout.bytesPerRow = 256;
        const wgpu::Extent3D one = {1, 1, 1};
        encoder.CopyTextureToBuffer(&source, &destination, &one);
    }

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
        encoder.CopyBufferToBuffer(indexed ? indexedArgs_ : args_, 0, readback->buffer,
                                   kQueries * 8, indexed ? 40 : 32);
    }
    const wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    const u64 submitted = ++frameCount_;
    const auto submittedAt = std::chrono::steady_clock::now();

    if (pick != nullptr) {
        pick->busy = true;
        pick->buffer.MapAsync(
            wgpu::MapMode::Read, 0, 8, callbackMode_,
            [this, pick, submitted, submittedAt, tag = frameTag_](wgpu::MapAsyncStatus status,
                                                                  wgpu::StringView) {
                if (status == wgpu::MapAsyncStatus::Success) {
                    u32 ids[2];
                    std::memcpy(ids, pick->buffer.GetConstMappedRange(0, 8), sizeof(ids));
                    PickResult result;
                    result.id = ids[0];
                    result.body = ids[1];
                    result.frames = u32(frameCount_ - submitted);
                    result.ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - submittedAt)
                                    .count();
                    result.tag = tag;
                    picks_.push_back(result);
                    pick->buffer.Unmap();
                }
                pick->busy = false;
            });
    }

    if (readback == nullptr) {
        return;
    }
    readback->busy = true;
    const Features features = features_;
    readback->buffer.MapAsync(
        wgpu::MapMode::Read, 0, kReadbackBytes, callbackMode_,
        [this, readback, indexed, features, tag = frameTag_](wgpu::MapAsyncStatus status,
                                                        wgpu::StringView) {
            if (status == wgpu::MapAsyncStatus::Success) {
                const auto* bytes =
                    static_cast<const u8*>(readback->buffer.GetConstMappedRange(0, kReadbackBytes));
                u64 t[kQueries];
                u32 args[10];
                std::memcpy(t, bytes, sizeof(t));
                std::memcpy(args, bytes + sizeof(t), sizeof(args));
                const auto span = [&](Pass pass) {
                    return double(t[pass * 2 + 1] - t[pass * 2]) * 1e-6;
                };
                FrameTiming timing;
                timing.tag = tag;
                if (querySet_) {
                    timing.cull = span(kPassCull);
                    timing.shadow = features.shadow ? span(kPassShadow) : 0;
                    timing.vis = span(kPassVis);
                    timing.occlusion = features.occlusion ? span(kPassOcclusion) : 0;
                    timing.composite = span(kPassComposite);
                    timing.total = double(t[kPassComposite * 2 + 1] - t[kPassCull * 2]) * 1e-6;
                }
                // Pulled: 16-triangle clusters. Indexed: visible triangles.
                timing.clusters = indexed ? (u64(args[0]) + u64(args[5])) / 3
                                          : (u64(args[0]) + u64(args[4])) / 48;
                if (std::getenv("SPIKE_RAW_TIMESTAMPS") != nullptr) {
                    for (u64 value : t) {
                        std::fprintf(stderr, "%llu ", value);
                    }
                    std::fprintf(stderr, "\n");
                }
                timings_.push_back(timing);
                readback->buffer.Unmap();
            }
            readback->busy = false;
        });
}

} // namespace geo::spike
