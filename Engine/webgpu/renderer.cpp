#include "webgpu/renderer.h"

#include "base/assert.h"
#include "render/style.h"
#include "scene/scene.h"
#include "shaders/shared/gpu_types.h"
#include "webgpu/wgsl.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace geo::webgpu {
namespace {

using U = wgpu::BufferUsage;
using Usage = wgpu::TextureUsage;
using Stage = wgpu::ShaderStage;
using Format = wgpu::TextureFormat;

// Uniform slots are this far apart: the frame's uniforms, rounded up to the 256 bytes any device
// may ask dynamic offsets to be aligned to.
constexpr u64 kUniformStride = (sizeof(FrameUniforms) + 255) & ~u64(255);
// Four jittered samples per frame, then the frame as it is, for culling, the shadow and the passes
// after, and one for classifying the shell.
constexpr u32 kFrameSlots = 6;
constexpr u32 kViewSlot = 4;
[[maybe_unused]] constexpr u32 kClassifySlot = 5; // native only: the browser cannot wait
constexpr u32 kListShadow = 6;
constexpr u32 kEdgeVertices = 192; // six per segment, for a chunk of 32

// Metal's standard 4x sample positions, from a pixel's top left. Sample 0 is drawn last, so its
// ids, tags and depth are left for the occlusion, the composite and picking, as Metal's are.
constexpr float kSamplePositions[4][2] = {
    {0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};

constexpr Format kColorFormat = Format::RGBA8Unorm;
constexpr Format kIdFormat = Format::R32Uint;
constexpr Format kDepthCopyFormat = Format::R32Float;
constexpr Format kInkFormat = Format::RGBA8Unorm;
constexpr Format kTagFormat = Format::RG8Uint;
constexpr Format kDepthFormat = Format::Depth32Float;
constexpr Format kShadowFormat = Format::R8Unorm;
constexpr Format kAccumFormat = Format::RGBA16Float;
constexpr Format kOcclusionFormat = Format::R32Float;

// The shaders' Definition: where a definition's streams start in the geometry buffer, in words.
struct GpuDefinitionW {
    u32 meshlets, vertices, highBits, triangles;
    u32 edgeSegments, edgeChunks, vertexCount, faceCount;
    float boundsMin[3];
    float boundsStep;
    float decodeScale[3];
    u32 flags;
    float boundsScale[3];
    u32 meshletCount;
};
static_assert(sizeof(GpuDefinitionW) == 80);

struct GpuState {
    u32 flags, facePage, look, pad;
};

struct Region {
    u32 slotBase, slotCapacity, indexBase, indexCapacity;
};

struct Lists {
    Region regions[8];
    Region edgeRegions[2];
    u32 chunks[8][4]; // per list: its first chunk draw, and how many it has
    u32 instanceCount, edgeInstanceCount, groupCount, shadowOn;
    u32 groupsBase, chunkDraws, pad0, pad1;
};
static_assert(sizeof(Lists) == 320);

struct DrawIndexed {
    u32 indexCount, instanceCount, firstIndex;
    i32 baseVertex;
    u32 firstInstance;
};

struct Draw {
    u32 vertexCount, instanceCount, firstVertex, firstInstance;
};

// The shaders' Counters, before its words: every chunk's draw, then what each workgroup adds to
// each list.
struct Counters {
    Draw edges[2];
    u32 edgeSlots[2];
    u32 slots[8];
    u32 indices[8];
};
static_assert(sizeof(Counters) == 104);
static_assert(sizeof(DrawIndexed) == 20);
constexpr u32 kLists = 7;
// A list is drawn in chunks of this many slots, so its indices fit 16 bits. See cull.wgsl.
constexpr u32 kChunkSlots = 512;
constexpr u64 kEdgeDrawOffset = offsetof(Counters, edges);

// What each surface list draws with: which kind of pass, and closed or open.
enum class Kind : u32 { Solid, Ghost, ShellBack, ShellDepth, ShellFront, Shadow, Edges, Classify };

struct PipelineKey {
    Kind kind = Kind::Solid;
    bool physical = false, section = false, closed = false, pickable = false;
    bool ghostLines = false, peel = false;

    u32 bits() const {
        return u32(kind) | u32(physical) << 4 | u32(section) << 5 | u32(closed) << 6 |
               u32(pickable) << 7 | u32(ghostLines) << 8 | u32(peel) << 9;
    }
};

wgpu::ShaderModule makeModule(const wgpu::Device& device, const std::string& code,
                              const char* label) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    wgpu::ShaderModuleDescriptor descriptor{};
    descriptor.nextInChain = &wgsl;
    descriptor.label = label;
    return device.CreateShaderModule(&descriptor);
}

wgpu::BindGroupLayoutEntry bufferLayout(u32 binding, wgpu::ShaderStage visibility,
                                        wgpu::BufferBindingType type, bool dynamic = false) {
    wgpu::BindGroupLayoutEntry entry{};
    entry.binding = binding;
    entry.visibility = visibility;
    entry.buffer.type = type;
    entry.buffer.hasDynamicOffset = dynamic;
    return entry;
}

wgpu::BindGroupLayoutEntry textureLayout(u32 binding, wgpu::ShaderStage visibility,
                                         wgpu::TextureSampleType type) {
    wgpu::BindGroupLayoutEntry entry{};
    entry.binding = binding;
    entry.visibility = visibility;
    entry.texture.sampleType = type;
    entry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
    return entry;
}

wgpu::BindGroupLayout makeLayout(const wgpu::Device& device,
                                 std::initializer_list<wgpu::BindGroupLayoutEntry> entries) {
    const std::vector<wgpu::BindGroupLayoutEntry> list(entries);
    wgpu::BindGroupLayoutDescriptor descriptor{};
    descriptor.entryCount = list.size();
    descriptor.entries = list.data();
    return device.CreateBindGroupLayout(&descriptor);
}

wgpu::BindGroupEntry bufferEntry(u32 binding, const wgpu::Buffer& buffer, u64 size = 0) {
    wgpu::BindGroupEntry entry{};
    entry.binding = binding;
    entry.buffer = buffer;
    if (size != 0) {
        entry.size = size;
    }
    return entry;
}

wgpu::BindGroupEntry textureEntry(u32 binding, const wgpu::Texture& texture) {
    wgpu::BindGroupEntry entry{};
    entry.binding = binding;
    entry.textureView = texture.CreateView();
    return entry;
}

wgpu::BindGroup makeGroup(const wgpu::Device& device, const wgpu::BindGroupLayout& layout,
                          std::initializer_list<wgpu::BindGroupEntry> entries) {
    const std::vector<wgpu::BindGroupEntry> list(entries);
    wgpu::BindGroupDescriptor descriptor{};
    descriptor.layout = layout;
    descriptor.entryCount = list.size();
    descriptor.entries = list.data();
    return device.CreateBindGroup(&descriptor);
}

wgpu::PipelineLayout makePipelineLayout(const wgpu::Device& device,
                                        std::initializer_list<wgpu::BindGroupLayout> groups) {
    const std::vector<wgpu::BindGroupLayout> list(groups);
    wgpu::PipelineLayoutDescriptor descriptor{};
    descriptor.bindGroupLayoutCount = list.size();
    descriptor.bindGroupLayouts = list.data();
    return device.CreatePipelineLayout(&descriptor);
}

wgpu::ConstantEntry constant(const char* key, double value) {
    wgpu::ConstantEntry entry{};
    entry.key = key;
    entry.value = value;
    return entry;
}

// Groups for dispatching `count` threads in workgroups of 64, two-dimensional past the limit.
std::array<u32, 2> dispatchSize(u64 count) {
    const u64 groups = (count + 63) / 64;
    const u32 x = u32(std::min<u64>(groups, 65535));
    return {x, x == 0 ? 0 : u32((groups + x - 1) / x)};
}

} // namespace

struct Renderer::State {
    wgpu::Instance instance;
    wgpu::Device device;
    wgpu::Queue queue;
    Format outputFormat = Format::BGRA8Unorm;
    wgpu::Limits limits{};

    wgpu::ShaderModule cullModule, surfaceModule, edgeModule, sheetModule, postModule;
    wgpu::ShaderModule overlayModule;
    wgpu::BindGroupLayout sceneLayout, cullLayout, surfaceLayout, peelLayout, edgeLayout;
    wgpu::BindGroupLayout sheetLayout, resolveLayout, occlusionLayout, compositeLayout;
    wgpu::PipelineLayout cullPipelineLayout, surfacePipelineLayout, peelPipelineLayout;
    wgpu::PipelineLayout edgePipelineLayout, sheetPipelineLayout, resolvePipelineLayout;
    wgpu::PipelineLayout occlusionPipelineLayout, compositePipelineLayout;
    wgpu::BindGroupLayout overlayLayout;
    wgpu::PipelineLayout overlayPipelineLayout;
    wgpu::RenderPipeline cube, quad;
    // The overlay: the atlas the host drew, and this frame's uniforms and quads.
    wgpu::Texture atlas;
    wgpu::Buffer overlayUniforms, overlayQuads;
    wgpu::BindGroup overlayGroup;
    float glyphAdvance = 0.0f;
    wgpu::ComputePipeline countSurfaces, scanSurfaces, writeSurfaces, chunkDraws, cullEdges;
    wgpu::ComputePipeline occlusion;
    wgpu::RenderPipeline sheet, resolve, composite;
    std::unordered_map<u32, wgpu::RenderPipeline> pipelines;
    wgpu::Sampler linearClamp;

    // The scene's tables. Geometry is every segment's bytes, one after another.
    wgpu::Buffer frames, styleBuffer, definitions, occurrences, geometry, instances;
    // Slots hold the surface lists' regions, then the edge lists'.
    wgpu::Buffer states, faceStates, palette, lists, counters, slots, indices;
    std::vector<const scene::Segment*> segments;
    struct OccurrenceInfo {
        u32 meshlets = 0, triangles = 0, edgeChunks = 0;
        bool open = false;
    };
    std::vector<OccurrenceInfo> occurrenceInfo;
    u32 surfaceInstances = 0, edgeInstances = 0;
    u64 triangles = 0, edgeSegments = 0;
    Lists listTable{};

    // What the states and looks last said, for planning the lists.
    std::vector<OccurrenceState> words;
    std::vector<u16> looks;
    u64 stateVersion = ~u64(0);
    bool hasGhosts = false, hasShells = false;

    // Natively, map callbacks run when the renderer processes events at each frame; in the
    // browser, whenever they arrive.
#ifdef __EMSCRIPTEN__
    static constexpr wgpu::CallbackMode kCallbacks = wgpu::CallbackMode::AllowSpontaneous;
#else
    static constexpr wgpu::CallbackMode kCallbacks = wgpu::CallbackMode::AllowProcessEvents;
#endif
    struct Readback {
        wgpu::Buffer buffer;
        bool busy = false;
    };
    // The hover probe: a window of ids and depths around where the engine last asked, and a
    // grid of samples across the view for the orbit's fallback pivots, read back from each frame
    // through a ring, so asking never waits for the GPU. See readPixel.
    static constexpr u32 kProbeSize = 64;
    static constexpr u32 kProbeSamples = 9;
    static constexpr u64 kProbePlane = u64(kProbeSize) * 256;
    static constexpr u64 kProbeBytes = 2 * kProbePlane + 2 * kProbeSamples * 256;
    struct Probe {
        i32 x0 = 0, y0 = 0;
        u32 width = 0, height = 0;
        std::vector<u32> ids;
        std::vector<float> depths;
        std::array<std::array<i32, 2>, kProbeSamples> samplePixels{};
        std::array<u32, kProbeSamples> sampleIds{};
        std::array<float, kProbeSamples> sampleDepths{};
    };
    std::array<Readback, 3> probeRing;
    Probe probe;
    // Readback callbacks hold this weakly, so one arriving after the renderer is gone, when
    // another processes the instance's events, does nothing.
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    mutable i32 probeX = -1, probeY = -1; // where the engine last asked
    u64 probesArrived = 0, probesFailed = 0;
    // GPU time per frame, from timestamps around the frame's passes, where the device has them.
    wgpu::QuerySet timestamps;
    wgpu::Buffer timestampResolve;
    std::array<Readback, 3> timingRing;
    double gpuMilliseconds = 0.0;
    u64 timedFrames = 0;

    StyleUniforms style{};
    bool physical = false;
    bool reducedDetail = false;
    bool drawsEdges = std::getenv("GEOMETRY_NO_EDGES") == nullptr;
    u32 samples = 1;

    // Targets for one size.
    u32 width = 0, height = 0;
    wgpu::Texture color, ids, depthCopy, ink, tags, depth, shadowMask, occlusionTexture, accum;
    wgpu::Texture offscreen;
    wgpu::BindGroup sceneGroup, cullGroup, surfaceGroup, edgeGroup, sheetGroup, resolveGroup;
    wgpu::BindGroup occlusionGroup, compositeGroup;

    wgpu::Buffer buffer(u64 size, wgpu::BufferUsage usage, const char* label) {
        wgpu::BufferDescriptor descriptor{};
        descriptor.label = label;
        descriptor.size = std::max<u64>((size + 15) & ~u64(15), 16);
        descriptor.usage = usage;
        return device.CreateBuffer(&descriptor);
    }

    wgpu::Texture texture(u32 w, u32 h, Format format, wgpu::TextureUsage usage,
                          const char* label) {
        wgpu::TextureDescriptor descriptor{};
        descriptor.label = label;
        descriptor.size = {std::max(w, 1u), std::max(h, 1u), 1};
        descriptor.format = format;
        descriptor.usage = usage;
        return device.CreateTexture(&descriptor);
    }

    bool createModules() {
        const std::string common = wgsl::kCommon;
        cullModule = makeModule(device, common + wgsl::kCull, "cull");
        surfaceModule = makeModule(device, common + wgsl::kSurface, "surface");
        edgeModule = makeModule(device, common + wgsl::kEdges, "edges");
        sheetModule = makeModule(device, common + wgsl::kSheet, "sheet");
        postModule = makeModule(device, common + wgsl::kPost, "post");
        overlayModule = makeModule(device, common + wgsl::kOverlay, "overlay");
        return cullModule && surfaceModule && edgeModule && sheetModule && postModule &&
               overlayModule;
    }

    void createLayouts() {
        using B = wgpu::BufferBindingType;
        using TS = wgpu::TextureSampleType;
        const Stage geometryStages = Stage::Vertex | Stage::Compute;
        const Stage all = Stage::Vertex | Stage::Fragment | Stage::Compute;
        sceneLayout = makeLayout(device, {
            bufferLayout(0, all, B::Uniform, true),
            bufferLayout(1, all, B::Uniform),
            bufferLayout(2, geometryStages, B::ReadOnlyStorage),
            bufferLayout(3, geometryStages, B::ReadOnlyStorage),
            bufferLayout(4, geometryStages, B::ReadOnlyStorage),
            bufferLayout(5, geometryStages, B::ReadOnlyStorage),
            bufferLayout(6, Stage::Vertex, B::ReadOnlyStorage),
            bufferLayout(7, Stage::Fragment, B::ReadOnlyStorage),
        });
        cullLayout = makeLayout(device, {
            bufferLayout(0, Stage::Compute, B::Uniform),
            bufferLayout(1, Stage::Compute, B::ReadOnlyStorage),
            bufferLayout(2, Stage::Compute, B::Storage),
            bufferLayout(3, Stage::Compute, B::Storage),
            bufferLayout(4, Stage::Compute, B::Storage),
        });
        surfaceLayout = makeLayout(device, {bufferLayout(0, Stage::Vertex, B::ReadOnlyStorage)});
        wgpu::BindGroupLayoutEntry frontDepths =
            textureLayout(2, Stage::Fragment, TS::UnfilterableFloat);
        peelLayout = makeLayout(device, {bufferLayout(0, Stage::Vertex, B::ReadOnlyStorage),
                                         textureLayout(1, Stage::Fragment, TS::Uint),
                                         frontDepths});
        edgeLayout = makeLayout(device, {bufferLayout(0, Stage::Vertex, B::ReadOnlyStorage)});
        wgpu::BindGroupLayoutEntry sampler{};
        sampler.binding = 1;
        sampler.visibility = Stage::Fragment;
        sampler.sampler.type = wgpu::SamplerBindingType::Filtering;
        sheetLayout = makeLayout(device, {textureLayout(0, Stage::Fragment, TS::Float), sampler});
        resolveLayout = makeLayout(device, {textureLayout(0, Stage::Fragment, TS::Float),
                                            textureLayout(1, Stage::Fragment, TS::Float),
                                            textureLayout(2, Stage::Fragment, TS::Uint)});
        wgpu::BindGroupLayoutEntry occlusionOut{};
        occlusionOut.binding = 5;
        occlusionOut.visibility = Stage::Compute;
        occlusionOut.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
        occlusionOut.storageTexture.format = kOcclusionFormat;
        occlusionOut.storageTexture.viewDimension = wgpu::TextureViewDimension::e2D;
        occlusionLayout = makeLayout(device, {textureLayout(2, Stage::Compute, TS::Uint),
                                              textureLayout(3, Stage::Compute,
                                                            TS::UnfilterableFloat),
                                              occlusionOut});
        // The occlusion is read without filtering, so it can be a 32-bit float.
        compositeLayout = makeLayout(device, {
            textureLayout(0, Stage::Fragment, TS::Float),
            textureLayout(2, Stage::Fragment, TS::Uint),
            textureLayout(3, Stage::Fragment, TS::UnfilterableFloat),
            textureLayout(4, Stage::Fragment, TS::UnfilterableFloat),
        });

        cullPipelineLayout = makePipelineLayout(device, {sceneLayout, cullLayout});
        surfacePipelineLayout = makePipelineLayout(device, {sceneLayout, surfaceLayout});
        peelPipelineLayout = makePipelineLayout(device, {sceneLayout, peelLayout});
        edgePipelineLayout = makePipelineLayout(device, {sceneLayout, edgeLayout});
        sheetPipelineLayout = makePipelineLayout(device, {sceneLayout, sheetLayout});
        resolvePipelineLayout = makePipelineLayout(device, {sceneLayout, resolveLayout});
        occlusionPipelineLayout = makePipelineLayout(device, {sceneLayout, occlusionLayout});
        compositePipelineLayout = makePipelineLayout(device, {sceneLayout, compositeLayout});
        wgpu::BindGroupLayoutEntry atlasSampler{};
        atlasSampler.binding = 3;
        atlasSampler.visibility = Stage::Fragment;
        atlasSampler.sampler.type = wgpu::SamplerBindingType::Filtering;
        overlayLayout = makeLayout(device, {
            bufferLayout(0, Stage::Vertex | Stage::Fragment, B::Uniform),
            bufferLayout(1, Stage::Vertex, B::ReadOnlyStorage),
            textureLayout(2, Stage::Fragment, TS::Float),
            atlasSampler,
        });
        overlayPipelineLayout = makePipelineLayout(device, {sceneLayout, overlayLayout});

        wgpu::SamplerDescriptor linear{};
        linear.magFilter = wgpu::FilterMode::Linear;
        linear.minFilter = wgpu::FilterMode::Linear;
        linearClamp = device.CreateSampler(&linear);
    }

    wgpu::ComputePipeline computePipeline(const wgpu::ShaderModule& module, const char* entry,
                                          const wgpu::PipelineLayout& layout,
                                          std::vector<wgpu::ConstantEntry> constants = {}) {
        wgpu::ComputePipelineDescriptor descriptor{};
        descriptor.label = entry;
        descriptor.layout = layout;
        descriptor.compute.module = module;
        descriptor.compute.entryPoint = entry;
        descriptor.compute.constantCount = constants.size();
        descriptor.compute.constants = constants.data();
        return device.CreateComputePipeline(&descriptor);
    }

    void createFixedPipelines() {
        countSurfaces = computePipeline(cullModule, "countSurfaces", cullPipelineLayout);
        scanSurfaces = computePipeline(cullModule, "scanSurfaces", cullPipelineLayout);
        writeSurfaces = computePipeline(cullModule, "writeSurfaces", cullPipelineLayout);
        chunkDraws = computePipeline(cullModule, "chunkDraws", cullPipelineLayout);
        cullEdges = computePipeline(cullModule, "cullEdges", cullPipelineLayout);
        occlusion = computePipeline(postModule, "ambientOcclusion", occlusionPipelineLayout);

        // The sheet only where nothing was drawn: at the far plane, depth 0 with reversed depth.
        wgpu::DepthStencilState farOnly{};
        farOnly.format = kDepthFormat;
        farOnly.depthWriteEnabled = wgpu::OptionalBool::False;
        farOnly.depthCompare = wgpu::CompareFunction::GreaterEqual;
        std::array<wgpu::ColorTargetState, 5> targets = mainTargets();
        for (u32 t = 1; t < 5; ++t) {
            targets[t].writeMask = wgpu::ColorWriteMask::None;
        }
        sheet = screenPipeline(sheetModule, "sheetFragment", sheetPipelineLayout, targets.data(),
                               5, &farOnly, "sheet");

        // Each sample adds its share into the accumulated colour.
        wgpu::BlendState share{};
        share.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::Constant,
                       wgpu::BlendFactor::One};
        share.alpha = share.color;
        wgpu::ColorTargetState accumTarget{};
        accumTarget.format = kAccumFormat;
        accumTarget.blend = &share;
        resolve = screenPipeline(postModule, "resolveFragment", resolvePipelineLayout,
                                 &accumTarget, 1, nullptr, "resolve");

        wgpu::ColorTargetState output{};
        output.format = outputFormat;
        composite = screenPipeline(postModule, "compositeFragment", compositePipelineLayout,
                                   &output, 1, nullptr, "composite");

        // The overlay takes away the ink beneath it, by its alpha; the cube writes pick ids and
        // clears the tags of what is behind it, and the scale bar's quads blend.
        wgpu::BlendState clearsInk{};
        clearsInk.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::Zero,
                           wgpu::BlendFactor::OneMinusSrcAlpha};
        clearsInk.alpha = clearsInk.color;
        wgpu::BlendState alphaBlend{};
        alphaBlend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
                            wgpu::BlendFactor::OneMinusSrcAlpha};
        alphaBlend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One,
                            wgpu::BlendFactor::OneMinusSrcAlpha};
        wgpu::DepthStencilState always{};
        always.format = kDepthFormat;
        always.depthWriteEnabled = wgpu::OptionalBool::False;
        always.depthCompare = wgpu::CompareFunction::Always;
        std::array<wgpu::ColorTargetState, 5> cubeTargets = mainTargets();
        cubeTargets[3].writeMask = wgpu::ColorWriteMask::All;
        cubeTargets[3].blend = &clearsInk;
        cubeTargets[4].writeMask = wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green;
        cube = overlayPipeline("cubeVertex", "cubeFragment", cubeTargets.data(), &always);
        std::array<wgpu::ColorTargetState, 5> quadTargets = mainTargets();
        quadTargets[0].blend = &alphaBlend;
        quadTargets[1].writeMask = wgpu::ColorWriteMask::None;
        quadTargets[2].writeMask = wgpu::ColorWriteMask::None;
        quadTargets[3].writeMask = wgpu::ColorWriteMask::All;
        quadTargets[3].blend = &clearsInk;
        quad = overlayPipeline("quadVertex", "quadFragment", quadTargets.data(), &always);
    }

    wgpu::RenderPipeline overlayPipeline(const char* vertex, const char* fragment,
                                         const wgpu::ColorTargetState* targets,
                                         const wgpu::DepthStencilState* depthState) {
        wgpu::FragmentState fragmentState{};
        fragmentState.module = overlayModule;
        fragmentState.entryPoint = fragment;
        fragmentState.targetCount = 5;
        fragmentState.targets = targets;
        wgpu::RenderPipelineDescriptor descriptor{};
        descriptor.label = vertex;
        descriptor.layout = overlayPipelineLayout;
        descriptor.vertex.module = overlayModule;
        descriptor.vertex.entryPoint = vertex;
        descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        descriptor.depthStencil = depthState;
        descriptor.fragment = &fragmentState;
        return device.CreateRenderPipeline(&descriptor);
    }

    wgpu::RenderPipeline screenPipeline(const wgpu::ShaderModule& module, const char* entry,
                                        const wgpu::PipelineLayout& layout,
                                        const wgpu::ColorTargetState* targets, u32 targetCount,
                                        const wgpu::DepthStencilState* depthState,
                                        const char* label) {
        wgpu::FragmentState fragment{};
        fragment.module = module;
        fragment.entryPoint = entry;
        fragment.targetCount = targetCount;
        fragment.targets = targets;
        wgpu::RenderPipelineDescriptor descriptor{};
        descriptor.label = label;
        descriptor.layout = layout;
        descriptor.vertex.module = module;
        descriptor.vertex.entryPoint = "screenVertex";
        descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        descriptor.depthStencil = depthState;
        descriptor.fragment = &fragment;
        return device.CreateRenderPipeline(&descriptor);
    }

    // The main pass's targets, as every pipeline drawn in it must declare them, in Metal's order:
    // colour, pick id, depth at the pixel's centre, edge ink and body tags, each written or not
    // as a pipeline chooses.
    static std::array<wgpu::ColorTargetState, 5> mainTargets() {
        std::array<wgpu::ColorTargetState, 5> targets{};
        targets[0].format = kColorFormat;
        targets[1].format = kIdFormat;
        targets[2].format = kDepthCopyFormat;
        targets[3].format = kInkFormat;
        targets[3].writeMask = wgpu::ColorWriteMask::None;
        targets[4].format = kTagFormat;
        targets[4].writeMask = wgpu::ColorWriteMask::None;
        return targets;
    }

    // A surface or edge pipeline, made the first time it is drawn with.
    const wgpu::RenderPipeline& pipeline(const PipelineKey& key) {
        const auto found = pipelines.find(key.bits());
        if (found != pipelines.end()) {
            return found->second;
        }
        return pipelines.emplace(key.bits(), makePipeline(key)).first->second;
    }

    wgpu::RenderPipeline makePipeline(const PipelineKey& key) {
        using CW = wgpu::ColorWriteMask;
        const bool edges = key.kind == Kind::Edges;
        // Each stage is given only the constants it reads: Safari fails to build a stage given one
        // it does not.
        u32 layer = 0;
        if (key.kind == Kind::ShellBack) {
            layer = 1;
        } else if (key.kind == Kind::ShellDepth) {
            layer = 2;
        } else if (key.kind == Kind::ShellFront) {
            layer = 3;
        }
        std::vector<wgpu::ConstantEntry> vertexConstants, fragmentConstants;
        if (edges) {
            vertexConstants = {constant("kGhostLinePass", key.ghostLines)};
            fragmentConstants = {constant("kGhostLinePass", key.ghostLines),
                                 constant("kSection", key.section)};
        } else {
            vertexConstants = {constant("kGhostPass", key.kind == Kind::Ghost),
                               constant("kShadowPass", key.kind == Kind::Shadow),
                               constant("kOccurrenceIds", key.kind == Kind::Classify),
                               constant("kShellLayer", layer)};
            if (key.kind == Kind::Shadow || key.kind == Kind::ShellDepth) {
                fragmentConstants = {constant("kSection", key.section)};
            } else if (key.kind == Kind::Classify) {
                fragmentConstants = {constant("kPeel", key.peel)};
            } else {
                fragmentConstants = {constant("kGhostPass", key.kind == Kind::Ghost),
                                     constant("kPhysical", key.physical),
                                     constant("kSection", key.section),
                                     constant("kShellLayer", layer)};
            }
        }

        wgpu::BlendState alphaBlend{};
        alphaBlend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
                            wgpu::BlendFactor::OneMinusSrcAlpha};
        alphaBlend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One,
                            wgpu::BlendFactor::OneMinusSrcAlpha};
        wgpu::BlendState keepLarger{};
        keepLarger.color = {wgpu::BlendOperation::Max, wgpu::BlendFactor::One,
                            wgpu::BlendFactor::One};
        keepLarger.alpha = keepLarger.color;

        std::array<wgpu::ColorTargetState, 5> targets = mainTargets();
        std::array<wgpu::ColorTargetState, 3> classifyTargets{};
        wgpu::ColorTargetState shadowTarget{};
        shadowTarget.format = kShadowFormat;
        const char* fragmentEntry = edges ? "edgeFragment" : "surfaceFragment";
        u32 targetCount = 5;
        const wgpu::ColorTargetState* targetList = targets.data();

        wgpu::DepthStencilState depthTest{};
        depthTest.format = kDepthFormat;
        depthTest.depthWriteEnabled = wgpu::OptionalBool::True;
        depthTest.depthCompare = wgpu::CompareFunction::Greater;
        const wgpu::DepthStencilState* depthState = &depthTest;
        wgpu::CullMode cull = key.closed ? wgpu::CullMode::Back : wgpu::CullMode::None;

        switch (key.kind) {
        case Kind::Solid:
            targets[4].writeMask = CW::Red;
            // Back faces seen through a cut are its caps.
            cull = key.closed && !key.section ? wgpu::CullMode::Back : wgpu::CullMode::None;
            break;
        case Kind::Ghost:
            targets[0].blend = &alphaBlend;
            targets[1].writeMask = key.pickable ? CW::All : CW::None;
            targets[2].writeMask = key.pickable ? CW::All : CW::None;
            targets[4].writeMask = CW::Green;
            depthTest.depthWriteEnabled = wgpu::OptionalBool::False;
            cull = key.closed && !key.section ? wgpu::CullMode::Back : wgpu::CullMode::None;
            break;
        case Kind::ShellBack:
            cull = wgpu::CullMode::Front;
            break;
        case Kind::ShellDepth:
            targets[0].writeMask = CW::None;
            targets[1].writeMask = CW::None;
            targets[2].writeMask = CW::None;
            fragmentEntry = "depthFragment";
            break;
        case Kind::ShellFront:
            // Blended once, at the depth the shell's nearest front faces left.
            targets[0].blend = &alphaBlend;
            targets[1].writeMask = CW::None;
            targets[2].writeMask = CW::None;
            targets[4].writeMask = CW::Green;
            depthTest.depthWriteEnabled = wgpu::OptionalBool::False;
            depthTest.depthCompare = wgpu::CompareFunction::Equal;
            break;
        case Kind::Shadow:
            fragmentEntry = "shadowFragment";
            targetList = &shadowTarget;
            targetCount = 1;
            depthState = nullptr;
            cull = wgpu::CullMode::None;
            break;
        case Kind::Edges:
            targets[0].writeMask = CW::None;
            targets[1].writeMask = CW::None;
            targets[2].writeMask = CW::None;
            targets[3].writeMask = CW::All;
            targets[3].blend = &keepLarger;
            depthTest.depthWriteEnabled = wgpu::OptionalBool::False;
            cull = wgpu::CullMode::None;
            break;
        case Kind::Classify:
            // Ids and the depth copy only, into targets of the classifying views' own.
            classifyTargets[0].format = Format::Undefined;
            classifyTargets[1].format = kIdFormat;
            classifyTargets[2].format = kDepthCopyFormat;
            targetList = classifyTargets.data();
            targetCount = 3;
            fragmentEntry = "classifyFragment";
            break;
        }

        wgpu::FragmentState fragment{};
        fragment.module = edges ? edgeModule : surfaceModule;
        fragment.entryPoint = fragmentEntry;
        fragment.targetCount = targetCount;
        fragment.targets = targetList;
        fragment.constantCount = fragmentConstants.size();
        fragment.constants = fragmentConstants.data();
        wgpu::RenderPipelineDescriptor descriptor{};
        // The classifying passes share the peel's layout: its fragment stage names the front's
        // textures, peeling or not.
        descriptor.layout =
            edges ? edgePipelineLayout
                  : (key.kind == Kind::Classify ? peelPipelineLayout : surfacePipelineLayout);
        descriptor.vertex.module = fragment.module;
        descriptor.vertex.entryPoint = edges ? "edgeVertex" : "surfaceVertex";
        descriptor.vertex.constantCount = vertexConstants.size();
        descriptor.vertex.constants = vertexConstants.data();
        descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        descriptor.primitive.frontFace = wgpu::FrontFace::CCW;
        descriptor.primitive.cullMode = cull;
        descriptor.depthStencil = depthState;
        descriptor.fragment = &fragment;
        return device.CreateRenderPipeline(&descriptor);
    }

    // ---- Tables -----------------------------------------------------------------------------

    void syncScene(const scene::Scene& scene) {
        std::vector<const scene::Segment*> present;
        for (u32 d = 0; d < scene.documentCount(); ++d) {
            for (const std::shared_ptr<scene::Segment>& segment : scene.document(d).segments()) {
                present.push_back(segment.get());
            }
        }
        // Each segment's bytes, one after another, so a definition is found by word offsets.
        std::unordered_map<const scene::Segment*, u64> wordBase;
        u64 bytes = 0;
        for (const scene::Segment* segment : present) {
            wordBase.emplace(segment, bytes / 4);
            bytes += (segment->size() + 15) & ~usize(15);
        }
        if (present != segments) {
            if (bytes > limits.maxStorageBufferBindingSize) {
                std::fprintf(stderr, "The scene's geometry is larger than the GPU can bind.\n");
                bytes = 0;
                wordBase.clear();
                present.clear();
            }
            geometry = buffer(bytes, U::Storage | U::CopyDst, "geometry");
            for (const scene::Segment* segment : present) {
                queue.WriteBuffer(geometry, wordBase.at(segment) * 4, segment->data(),
                                  segment->size());
            }
            segments = present;
        }

        std::vector<GpuDefinitionW> gpuDefinitions;
        std::vector<GpuOccurrence> gpuOccurrences;
        std::vector<std::array<u32, 2>> surfaceList, edgeList;
        occurrenceInfo.clear();
        u32 definitionBase = 0, idBase = 0;
        triangles = 0;
        edgeSegments = 0;
        for (u32 d = 0; d < scene.documentCount(); ++d) {
            const scene::Document& document = scene.document(d);
            if (!document.hasModel()) {
                continue;
            }
            const format::SegmentView& model = document.model();
            for (u32 i = 0; i < model.definitionInfos.size(); ++i) {
                GpuDefinitionW out{};
                const format::DefinitionRecord* record = document.geometry(i);
                const scene::Segment* segment = document.geometrySegment(i);
                if (record != nullptr && wordBase.contains(segment)) {
                    const format::SegmentView& view = segment->view();
                    const auto* base = static_cast<const u8*>(segment->data());
                    const u64 first = wordBase.at(segment);
                    const auto wordOf = [&](const void* pointer) {
                        const u64 offset = u64(static_cast<const u8*>(pointer) - base);
                        GEO_ASSERT(offset % 4 == 0);
                        return u32(first + offset / 4);
                    };
                    out.meshlets = wordOf(view.meshlets.data() + record->firstMeshlet);
                    out.vertices = wordOf(view.vertices.data() + record->firstVertex);
                    out.highBits = (record->flags & kDefinitionWide) != 0
                                       ? wordOf(view.highBits.data() + record->firstHighBits)
                                       : 0;
                    out.triangles = wordOf(view.triangles.data() + record->firstTriangle);
                    out.edgeSegments =
                        wordOf(view.edgeSegments.data() + record->firstEdgeSegment);
                    out.edgeChunks = wordOf(view.edgeChunks.data() + record->firstEdgeChunk);
                    out.vertexCount = record->vertexCount;
                    out.faceCount = record->faceCount;
                    out.meshletCount = record->meshletCount;
                    out.flags = record->flags;
                    for (int a = 0; a < 3; ++a) {
                        out.boundsMin[a] = record->boundsMin[a];
                        out.decodeScale[a] = record->decodeScale[a];
                        out.boundsScale[a] = record->boundsScale[a];
                    }
                    out.boundsStep =
                        std::max({record->boundsScale[0], record->boundsScale[1],
                                  record->boundsScale[2]});
                }
                gpuDefinitions.push_back(out);
            }
            for (u32 i = 0; i < model.occurrences.size(); ++i) {
                const format::OccurrenceRecord& record = model.occurrences[i];
                const std::array<float, 12>& t = record.transform;
                const u32 global = u32(gpuOccurrences.size());
                GpuOccurrence out{};
                out.rows[0] = simd_make_float4(t[0], t[3], t[6], t[9]);
                out.rows[1] = simd_make_float4(t[1], t[4], t[7], t[10]);
                out.rows[2] = simd_make_float4(t[2], t[5], t[8], t[11]);
                out.definition = definitionBase + record.definition;
                out.idBase = idBase + document.idBase(i);
                gpuOccurrences.push_back(out);
                OccurrenceInfo info;
                const format::DefinitionRecord* geometryRecord =
                    document.geometry(record.definition);
                if (geometryRecord != nullptr &&
                    wordBase.contains(document.geometrySegment(record.definition))) {
                    info.meshlets = geometryRecord->meshletCount;
                    info.triangles = geometryRecord->triangleCount;
                    info.edgeChunks = geometryRecord->edgeChunkCount;
                    info.open = (geometryRecord->flags & kDefinitionOpen) != 0;
                    triangles += geometryRecord->triangleCount;
                    edgeSegments += geometryRecord->edgeSegmentCount;
                    for (u32 m = 0; m < info.meshlets; ++m) {
                        surfaceList.push_back({global, m});
                    }
                    for (u32 c = 0; c < info.edgeChunks; ++c) {
                        edgeList.push_back({global, c});
                    }
                }
                occurrenceInfo.push_back(info);
            }
            definitionBase += u32(model.definitionInfos.size());
            idBase += document.idCount();
        }
        definitions = buffer(gpuDefinitions.size() * sizeof(GpuDefinitionW),
                             U::Storage | U::CopyDst, "definitions");
        queue.WriteBuffer(definitions, 0, gpuDefinitions.data(),
                          gpuDefinitions.size() * sizeof(GpuDefinitionW));
        occurrences = buffer(gpuOccurrences.size() * sizeof(GpuOccurrence),
                             U::Storage | U::CopyDst, "occurrences");
        queue.WriteBuffer(occurrences, 0, gpuOccurrences.data(),
                          gpuOccurrences.size() * sizeof(GpuOccurrence));
        surfaceInstances = u32(surfaceList.size());
        edgeInstances = u32(edgeList.size());
        surfaceList.insert(surfaceList.end(), edgeList.begin(), edgeList.end());
        instances = buffer(surfaceList.size() * 8, U::Storage | U::CopyDst, "instances");
        queue.WriteBuffer(instances, 0, surfaceList.data(), surfaceList.size() * 8);

        cullGroup = nullptr; // it binds the instances
        // States and looks follow the scene's occurrences, until synced again.
        words.assign(gpuOccurrences.size(), OccurrenceState{0, 0});
        looks.assign(gpuOccurrences.size(), 0);
        faceStates = buffer(16, U::Storage | U::CopyDst, "face states");
        stateVersion = ~u64(0);
        uploadStates();
        planLists();
    }

    void uploadStates() {
        std::vector<GpuState> gpu(std::max<usize>(words.size(), 1));
        hasGhosts = false;
        hasShells = false;
        for (usize o = 0; o < words.size(); ++o) {
            const u16 look = o < looks.size() ? looks[o] : 0;
            gpu[o] = {words[o].flags, words[o].facePage, look, 0};
            hasGhosts = hasGhosts || (words[o].flags & (kStateGhost | kStateFaceGhost)) != 0;
            hasShells = hasShells || (look & kLookShell) != 0;
        }
        states = buffer(gpu.size() * sizeof(GpuState), U::Storage | U::CopyDst, "states");
        queue.WriteBuffer(states, 0, gpu.data(), gpu.size() * sizeof(GpuState));
        sceneGroup = nullptr;
    }

    // Sizes each list's region for the most it could hold given the states and looks, so a
    // frame's cull never overflows one.
    void planLists() {
        std::array<u64, 8> slotNeed{}, indexNeed{};
        std::array<u64, 2> edgeNeed{};
        const bool shadow = style.shadowOn != 0;
        for (usize o = 0; o < occurrenceInfo.size(); ++o) {
            const OccurrenceInfo& info = occurrenceInfo[o];
            const u32 flags = o < words.size() ? words[o].flags : 0;
            if ((flags & kStateHidden) != 0 || info.meshlets == 0) {
                continue;
            }
            const u32 open = info.open ? 1 : 0;
            const bool ghostWhole = (flags & kStateGhost) != 0;
            const bool ghost = (flags & (kStateGhost | kStateFaceGhost)) != 0;
            const bool shell = o < looks.size() && (looks[o] & kLookShell) != 0;
            // Each meshlet may add an empty triangle, to an even number of indices.
            const auto need = [&](u32 list) {
                slotNeed[list] += info.meshlets;
                indexNeed[list] += (u64(info.triangles) + info.meshlets) * 3;
            };
            if (!ghostWhole) {
                need(open);
            }
            if (ghost) {
                need(2 + open);
            }
            if (shell && !ghostWhole) {
                need(4 + open);
            }
            if (shadow && !ghostWhole) {
                need(kListShadow);
            }
            edgeNeed[0] += info.edgeChunks;
            if (ghost) {
                edgeNeed[1] += info.edgeChunks;
            }
        }
        Lists table{};
        u64 slotTotal = 0, indexTotal = 0, edgeTotal = 0;
        u32 chunkTotal = 0;
        for (u32 list = 0; list < 8; ++list) {
            indexNeed[list] = (indexNeed[list] + 1) & ~u64(1); // regions start on whole words
            table.regions[list] = {u32(slotTotal), u32(slotNeed[list]), u32(indexTotal),
                                   u32(indexNeed[list])};
            const u32 chunks = list < kLists ? u32((slotNeed[list] + kChunkSlots - 1) / kChunkSlots)
                                             : 0;
            table.chunks[list][0] = chunkTotal;
            table.chunks[list][1] = chunks;
            chunkTotal += chunks;
            slotTotal += slotNeed[list];
            indexTotal += indexNeed[list];
        }
        table.chunkDraws = chunkTotal;
        table.groupsBase = chunkTotal * 5;
        for (u32 list = 0; list < 2; ++list) {
            table.edgeRegions[list] = {u32(slotTotal + edgeTotal), u32(edgeNeed[list]), 0, 0};
            edgeTotal += edgeNeed[list];
        }
        table.instanceCount = surfaceInstances;
        table.edgeInstanceCount = edgeInstances;
        const auto [groupsX, groupsY] = dispatchSize(surfaceInstances);
        table.groupCount = groupsX * groupsY;
        table.shadowOn = listTable.shadowOn;
        listTable = table;
        if (!lists) {
            lists = buffer(sizeof(Lists), U::Uniform | U::CopyDst, "lists");
        }
        queue.WriteBuffer(lists, 0, &table, sizeof(table));
        const auto grow = [&](wgpu::Buffer& target, u64 size, wgpu::BufferUsage usage,
                              const char* label) {
            if (!target || target.GetSize() < size) {
                target = buffer(std::max<u64>(size, 16), usage, label);
                cullGroup = nullptr;
                surfaceGroup = nullptr;
                edgeGroup = nullptr;
                peelGroup = nullptr;
            }
        };
        grow(slots, (slotTotal + edgeTotal) * 8, U::Storage, "slots");
        grow(counters,
             sizeof(Counters) + (u64(table.groupsBase) + u64(table.groupCount) * kLists * 2) * 4,
             U::Storage | U::Indirect | U::CopyDst, "counters");
        grow(indices, indexTotal * 2, U::Storage | U::Index, "indices");
        if (indexTotal * 2 > limits.maxStorageBufferBindingSize) {
            std::fprintf(stderr, "The scene's draw lists are larger than the GPU can bind.\n");
        }
    }

    // ---- Bind groups ---------------------------------------------------------------------

    void ensureGroups() {
        if (!frames) {
            frames = buffer(kUniformStride * kFrameSlots, U::Uniform | U::CopyDst, "frames");
        }
        if (!styleBuffer) {
            styleBuffer = buffer(sizeof(StyleUniforms), U::Uniform | U::CopyDst, "style");
        }
        if (!palette) {
            const std::array<u32, 3> neutral = {0xFFF3FAFBu, 0xFFD2E0E3u, 0xFFA6B8BDu};
            palette = buffer(sizeof(neutral), U::Storage | U::CopyDst, "palette");
            queue.WriteBuffer(palette, 0, neutral.data(), sizeof(neutral));
        }
        if (!definitions) {
            definitions = buffer(80, U::Storage | U::CopyDst, "definitions");
            occurrences = buffer(64, U::Storage | U::CopyDst, "occurrences");
            geometry = buffer(16, U::Storage | U::CopyDst, "geometry");
            instances = buffer(16, U::Storage | U::CopyDst, "instances");
            faceStates = buffer(16, U::Storage | U::CopyDst, "face states");
            uploadStates();
            planLists();
        }
        if (!sceneGroup) {
            sceneGroup = makeGroup(device, sceneLayout, {
                bufferEntry(0, frames, sizeof(FrameUniforms)),
                bufferEntry(1, styleBuffer),
                bufferEntry(2, definitions),
                bufferEntry(3, occurrences),
                bufferEntry(4, geometry),
                bufferEntry(5, states),
                bufferEntry(6, faceStates),
                bufferEntry(7, palette),
            });
        }
        if (!cullGroup) {
            cullGroup = makeGroup(device, cullLayout, {
                bufferEntry(0, lists),
                bufferEntry(1, instances),
                bufferEntry(2, slots),
                bufferEntry(3, indices),
                bufferEntry(4, counters),
            });
        }
        if (!surfaceGroup) {
            surfaceGroup = makeGroup(device, surfaceLayout, {bufferEntry(0, slots)});
        }
        if (!edgeGroup) {
            edgeGroup = makeGroup(device, edgeLayout, {bufferEntry(0, slots)});
        }
    }

    // ---- Targets ---------------------------------------------------------------------------

    void resize(u32 w, u32 h) {
        if (w == width && h == height && color) {
            return;
        }
        width = std::max(w, 1u);
        height = std::max(h, 1u);
        const Usage attachment = Usage::RenderAttachment | Usage::TextureBinding;
        color = texture(width, height, kColorFormat, attachment, "colour");
        ids = texture(width, height, kIdFormat, attachment | Usage::CopySrc, "ids");
        depthCopy = texture(width, height, kDepthCopyFormat, attachment | Usage::CopySrc,
                            "depth copy");
        ink = texture(width, height, kInkFormat, attachment, "ink");
        tags = texture(width, height, kTagFormat, attachment, "tags");
        depth = texture(width, height, kDepthFormat, Usage::RenderAttachment, "depth");
        const u32 halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
        shadowMask = texture(halfWidth, halfHeight, kShadowFormat, attachment, "shadow mask");
        occlusionTexture = texture(halfWidth, halfHeight, kOcclusionFormat,
                                   Usage::StorageBinding | Usage::TextureBinding, "occlusion");
        accum = texture(width, height, kAccumFormat, attachment, "accumulated colour");
        offscreen = texture(width, height, outputFormat,
                            Usage::RenderAttachment | Usage::CopySrc, "offscreen");
        sheetGroup = makeGroup(device, sheetLayout, {textureEntry(0, shadowMask), [&] {
                                   wgpu::BindGroupEntry entry{};
                                   entry.binding = 1;
                                   entry.sampler = linearClamp;
                                   return entry;
                               }()});
        resolveGroup = makeGroup(device, resolveLayout, {textureEntry(0, color),
                                                         textureEntry(1, ink),
                                                         textureEntry(2, tags)});
        occlusionGroup = makeGroup(device, occlusionLayout, {textureEntry(2, tags),
                                                             textureEntry(3, depthCopy),
                                                             textureEntry(5, occlusionTexture)});
        compositeGroup = makeGroup(device, compositeLayout, {textureEntry(0, accum),
                                                             textureEntry(2, tags),
                                                             textureEntry(3, depthCopy),
                                                             textureEntry(4, occlusionTexture)});
    }

    // ---- Frames ----------------------------------------------------------------------------

    bool castsShadow(const render::ViewState& view) const {
        return surfaceInstances != 0 && !reducedDetail && style.shadowOn != 0 &&
               view.uniforms.shadowVisibility > 0.0f;
    }

    // This sample's frame uniforms: the view moved by a fraction of a pixel, so the pixel's
    // centre falls where Metal's sample would.
    void writeFrame(u32 slot, const FrameUniforms& base, const float* position) {
        FrameUniforms uniforms = base;
        if (position != nullptr) {
            const float dx = -2.0f * (position[0] - 0.5f) / float(width);
            const float dy = 2.0f * (position[1] - 0.5f) / float(height);
            for (int c = 0; c < 4; ++c) {
                uniforms.viewProjection.columns[c][0] += dx * uniforms.viewProjection.columns[c][3];
                uniforms.viewProjection.columns[c][1] += dy * uniforms.viewProjection.columns[c][3];
            }
            uniforms.sampleOffset = simd_make_float2(position[0] - 0.5f, position[1] - 0.5f);
        }
        queue.WriteBuffer(frames, slot * kUniformStride, &uniforms, sizeof(uniforms));
    }

    // Zero counts, and each chunk's draw with its base vertex: its first slot << 7, so the
    // vertex stage sees the slot whole. The cull fills in where indices start and how many.
    void resetCounters(bool shadow) {
        Counters reset{};
        for (u32 list = 0; list < 2; ++list) {
            reset.edges[list] = {0, 1, listTable.edgeRegions[list].slotBase * kEdgeVertices, 0};
        }
        resetDraws.resize(listTable.chunkDraws);
        for (u32 list = 0; list < kLists; ++list) {
            const Region& region = listTable.regions[list];
            for (u32 k = 0; k < listTable.chunks[list][1]; ++k) {
                resetDraws[listTable.chunks[list][0] + k] = {
                    0, 1, region.indexBase, i32((region.slotBase + k * kChunkSlots) << 7), 0};
            }
        }
        queue.WriteBuffer(counters, 0, &reset, sizeof(reset));
        queue.WriteBuffer(counters, sizeof(Counters), resetDraws.data(),
                          resetDraws.size() * sizeof(DrawIndexed));
        if (listTable.shadowOn != u32(shadow)) {
            listTable.shadowOn = shadow;
            queue.WriteBuffer(lists, offsetof(Lists, shadowOn), &listTable.shadowOn, 4);
        }
    }
    std::vector<DrawIndexed> resetDraws;

    // The shadow's list fills in the same passes, when resetCounters said it is on.
    void encodeCull(const wgpu::CommandEncoder& encoder, bool edges, u32 slot = kViewSlot) {
        const u32 offset = u32(slot * kUniformStride);
        wgpu::PassTimestampWrites begin{};
        wgpu::ComputePassDescriptor descriptor{};
        if (timestamps && slot == kViewSlot) {
            // Both ends of each timed pass are written: the browser's WebGPU does not take an
            // index left undefined.
            begin.querySet = timestamps;
            begin.beginningOfPassWriteIndex = 0;
            begin.endOfPassWriteIndex = 1;
            descriptor.timestampWrites = &begin;
        }
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass(&descriptor);
        pass.SetBindGroup(0, sceneGroup, 1, &offset);
        pass.SetBindGroup(1, cullGroup);
        if (surfaceInstances != 0) {
            const auto [x, y] = dispatchSize(surfaceInstances);
            pass.SetPipeline(countSurfaces);
            pass.DispatchWorkgroups(x, y);
            pass.SetPipeline(scanSurfaces);
            pass.DispatchWorkgroups(1);
            pass.SetPipeline(writeSurfaces);
            pass.DispatchWorkgroups(x, y);
            pass.SetPipeline(chunkDraws);
            pass.DispatchWorkgroups((listTable.chunkDraws + 63) / 64);
        }
        if (edges && edgeInstances != 0) {
            const auto [x, y] = dispatchSize(edgeInstances);
            pass.SetPipeline(cullEdges);
            pass.DispatchWorkgroups(x, y);
        }
        pass.End();
    }

    // A list's chunks, each its own indexed draw; a chunk the cull left empty draws nothing.
    void drawList(const wgpu::RenderPassEncoder& pass, u32 list) {
        const u32 first = listTable.chunks[list][0], count = listTable.chunks[list][1];
        for (u32 k = 0; k < count; ++k) {
            pass.DrawIndexedIndirect(counters, sizeof(Counters) + (first + k) * sizeof(DrawIndexed));
        }
    }

    // Closed surfaces, then open ones, of a pair of lists, with the pipelines for each.
    void drawPair(const wgpu::RenderPassEncoder& pass, PipelineKey key, u32 closedList) {
        key.closed = true;
        pass.SetPipeline(pipeline(key));
        drawList(pass, closedList);
        key.closed = false;
        pass.SetPipeline(pipeline(key));
        drawList(pass, closedList + 1);
    }

    void encodeShadow(const wgpu::CommandEncoder& encoder, bool section) {
        wgpu::RenderPassColorAttachment attachment{};
        attachment.view = shadowMask.CreateView();
        attachment.loadOp = wgpu::LoadOp::Clear;
        attachment.storeOp = wgpu::StoreOp::Store;
        attachment.clearValue = {0, 0, 0, 0};
        wgpu::RenderPassDescriptor descriptor{};
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &attachment;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&descriptor);
        const u32 offset = u32(kViewSlot * kUniformStride);
        pass.SetBindGroup(0, sceneGroup, 1, &offset);
        pass.SetBindGroup(1, surfaceGroup);
        pass.SetIndexBuffer(indices, wgpu::IndexFormat::Uint16);
        PipelineKey key{.kind = Kind::Shadow, .section = section};
        pass.SetPipeline(pipeline(key));
        drawList(pass, kListShadow);
        pass.End();
    }

    // The main pass for one sample: opaque surfaces, the sheet behind them, creases and
    // highlights, the see-through shell, ghosts over all of it with their dashed lines.
    void encodeMain(const wgpu::CommandEncoder& encoder, u32 slot, const render::ViewState& view,
                    const render::Overlay* overlay) {
        const bool section = view.uniforms.sectionPlaneCount > 0;
        wgpu::RenderPassColorAttachment attachments[5] = {};
        const wgpu::Texture* textures[5] = {&color, &ids, &depthCopy, &ink, &tags};
        for (u32 t = 0; t < 5; ++t) {
            attachments[t].view = textures[t]->CreateView();
            attachments[t].loadOp = wgpu::LoadOp::Clear;
            attachments[t].storeOp = wgpu::StoreOp::Store;
            attachments[t].clearValue = {0, 0, 0, 0};
        }
        wgpu::RenderPassDepthStencilAttachment depthAttachment{};
        depthAttachment.view = depth.CreateView();
        depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
        depthAttachment.depthStoreOp = wgpu::StoreOp::Store;
        depthAttachment.depthClearValue = 0.0f; // reversed depth
        wgpu::RenderPassDescriptor descriptor{};
        descriptor.colorAttachmentCount = 5;
        descriptor.colorAttachments = attachments;
        descriptor.depthStencilAttachment = &depthAttachment;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&descriptor);
        const u32 offset = u32(slot * kUniformStride);
        pass.SetBindGroup(0, sceneGroup, 1, &offset);

        const bool hasScene = surfaceInstances != 0;
        if (hasScene) {
            pass.SetBindGroup(1, surfaceGroup);
            pass.SetIndexBuffer(indices, wgpu::IndexFormat::Uint16);
            drawPair(pass, {.kind = Kind::Solid, .physical = physical, .section = section}, 0);
        }
        const bool seeThrough = hasScene && hasShells && view.uniforms.seeThrough > 0.0f;
        if (seeThrough) {
            // The shell's far walls: back faces, held just in front of the sheet by a depth
            // range of their own, so they always lie under the mechanism.
            pass.SetViewport(0, 0, float(width), float(height), 1e-6f, 1e-4f);
            drawPair(pass, {.kind = Kind::ShellBack, .physical = physical, .section = section}, 4);
            pass.SetViewport(0, 0, float(width), float(height), 0.0f, 1.0f);
        }

        // The sheet only where nothing was drawn.
        pass.SetPipeline(sheet);
        pass.SetBindGroup(1, sheetGroup);
        pass.Draw(3);
        if (hasScene && drawsEdges && !reducedDetail) {
            pass.SetBindGroup(1, edgeGroup);
            pass.SetPipeline(pipeline({.kind = Kind::Edges, .section = section}));
            pass.DrawIndirect(counters, kEdgeDrawOffset);
        }
        if (hasScene) {
            pass.SetBindGroup(1, surfaceGroup);
        }
        if (seeThrough) {
            // The shell's nearest front faces, over everything opaque: into depth, then blended
            // once.
            drawPair(pass, {.kind = Kind::ShellDepth, .section = section}, 4);
            drawPair(pass, {.kind = Kind::ShellFront, .physical = physical, .section = section}, 4);
        }
        // Ghosts last, blended over everything opaque and writing no depth.
        if (hasScene && hasGhosts) {
            drawPair(pass, {.kind = Kind::Ghost, .physical = physical, .section = section,
                            .pickable = view.uniforms.ghostPickable != 0},
                     2);
            if ((style.ghostStyle & kGhostLines) != 0 && drawsEdges && !reducedDetail) {
                pass.SetBindGroup(1, edgeGroup);
                pass.SetPipeline(
                    pipeline({.kind = Kind::Edges, .section = section, .ghostLines = true}));
                pass.DrawIndirect(counters, kEdgeDrawOffset + sizeof(Draw));
            }
        }
        if (overlay != nullptr && overlay->visible && overlayGroup) {
            pass.SetBindGroup(1, overlayGroup);
            pass.SetPipeline(cube);
            pass.Draw(36);
            if (overlay->uniforms.quadCount != 0) {
                pass.SetPipeline(quad);
                pass.Draw(6, overlay->uniforms.quadCount);
            }
        }
        pass.End();
    }

    // Inks this sample and adds its share into the accumulated colour.
    void encodeResolve(const wgpu::CommandEncoder& encoder, bool first, u32 count) {
        wgpu::RenderPassColorAttachment attachment{};
        attachment.view = accum.CreateView();
        attachment.loadOp = first ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        attachment.storeOp = wgpu::StoreOp::Store;
        attachment.clearValue = {0, 0, 0, 0};
        wgpu::RenderPassDescriptor descriptor{};
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &attachment;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&descriptor);
        const u32 offset = u32(kViewSlot * kUniformStride);
        pass.SetPipeline(resolve);
        pass.SetBindGroup(0, sceneGroup, 1, &offset);
        pass.SetBindGroup(1, resolveGroup);
        const double share = 1.0 / double(count);
        const wgpu::Color constantColor = {share, share, share, share};
        pass.SetBlendConstant(&constantColor);
        pass.Draw(3);
        pass.End();
    }

    void encodeFrame(const wgpu::Texture& target, const render::ViewState& view, u32 count,
                     const render::Overlay* overlay) {
#ifndef __EMSCRIPTEN__
        instance.ProcessEvents(); // earlier frames' readbacks
#endif
        ensureGroups();
        queue.WriteBuffer(styleBuffer, 0, &style, sizeof(style));
        for (u32 s = 0; s < count; ++s) {
            writeFrame(s, view.uniforms, count > 1 ? kSamplePositions[s] : nullptr);
        }
        writeFrame(kViewSlot, view.uniforms, nullptr);
        if (overlay != nullptr && overlay->visible && overlayGroup) {
            OverlayUniforms uniforms = overlay->uniforms;
            uniforms.quadCount = std::min<u32>(uniforms.quadCount, kOverlayMaxQuads);
            queue.WriteBuffer(overlayUniforms, 0, &uniforms, sizeof(uniforms));
            queue.WriteBuffer(overlayQuads, 0, overlay->quads.data(),
                              uniforms.quadCount * sizeof(OverlayQuad));
        }
        const bool section = view.uniforms.sectionPlaneCount > 0;
        const bool shadow = castsShadow(view);
        resetCounters(shadow);
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        encodeCull(encoder, true);
        if (shadow) {
            encodeShadow(encoder, section);
        }
        // Sample 0 last, so its ids, tags and depth remain.
        for (u32 n = 0; n < count; ++n) {
            const u32 s = (n + 1) % count;
            encodeMain(encoder, s, view, overlay);
            encodeResolve(encoder, n == 0, count);
        }
        if (style.ao > 0.0f) {
            wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
            const u32 offset = u32(kViewSlot * kUniformStride);
            pass.SetPipeline(occlusion);
            pass.SetBindGroup(0, sceneGroup, 1, &offset);
            pass.SetBindGroup(1, occlusionGroup);
            pass.DispatchWorkgroups(((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8);
            pass.End();
        }
        wgpu::RenderPassColorAttachment output{};
        output.view = target.CreateView();
        output.loadOp = wgpu::LoadOp::Clear;
        output.storeOp = wgpu::StoreOp::Store;
        output.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor descriptor{};
        descriptor.colorAttachmentCount = 1;
        descriptor.colorAttachments = &output;
        wgpu::PassTimestampWrites end{};
        if (timestamps) {
            end.querySet = timestamps;
            end.beginningOfPassWriteIndex = 2;
            end.endOfPassWriteIndex = 3;
            descriptor.timestampWrites = &end;
        }
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&descriptor);
        const u32 offset = u32(kViewSlot * kUniformStride);
        pass.SetPipeline(composite);
        pass.SetBindGroup(0, sceneGroup, 1, &offset);
        pass.SetBindGroup(1, compositeGroup);
        pass.Draw(3);
        pass.End();
        Readback* probeReadback = encodeProbe(encoder);
        Readback* timingReadback = nullptr;
        if (timestamps) {
            encoder.ResolveQuerySet(timestamps, 0, 4, timestampResolve, 0);
            timingReadback = freeReadback(timingRing, 32, "timing");
            if (timingReadback != nullptr) {
                encoder.CopyBufferToBuffer(timestampResolve, 0, timingReadback->buffer, 0, 32);
            }
        }
        const wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        if (probeReadback != nullptr) {
            mapProbe(*probeReadback);
        }
        if (timingReadback != nullptr) {
            mapTiming(*timingReadback);
        }
    }

    // A readback of the ring not waiting on the GPU, or none if all are.
    Readback* freeReadback(std::array<Readback, 3>& ring, u64 size, const char* label) {
        for (Readback& readback : ring) {
            if (!readback.busy) {
                if (!readback.buffer) {
                    readback.buffer = buffer(size, U::MapRead | U::CopyDst, label);
                }
                readback.busy = true;
                return &readback;
            }
        }
        return nullptr;
    }

    // Copies the probe's window and samples out of sample 0's ids and depths.
    Readback* encodeProbe(const wgpu::CommandEncoder& encoder) {
        Readback* readback = freeReadback(probeRing, kProbeBytes, "probe");
        if (readback == nullptr) {
            return nullptr;
        }
        const u32 w = std::min(kProbeSize, width), h = std::min(kProbeSize, height);
        const i32 cx = probeX >= 0 ? probeX : i32(width / 2);
        const i32 cy = probeY >= 0 ? probeY : i32(height / 2);
        const u32 x0 = u32(std::clamp(cx - i32(w / 2), 0, i32(width - w)));
        const u32 y0 = u32(std::clamp(cy - i32(h / 2), 0, i32(height - h)));
        const auto copy = [&](const wgpu::Texture& texture, u32 x, u32 y, u32 cw, u32 ch,
                              u64 offset) {
            wgpu::TexelCopyTextureInfo source{};
            source.texture = texture;
            source.origin = {x, y, 0};
            wgpu::TexelCopyBufferInfo destination{};
            destination.buffer = readback->buffer;
            destination.layout.offset = offset;
            destination.layout.bytesPerRow = 256;
            destination.layout.rowsPerImage = ch;
            const wgpu::Extent3D extent = {cw, ch, 1};
            encoder.CopyTextureToBuffer(&source, &destination, &extent);
        };
        copy(ids, x0, y0, w, h, 0);
        copy(depthCopy, x0, y0, w, h, kProbePlane);
        Probe next;
        next.x0 = i32(x0);
        next.y0 = i32(y0);
        next.width = w;
        next.height = h;
        for (u32 k = 0; k < kProbeSamples; ++k) {
            const u32 x = width * (1 + 2 * (k % 3)) / 6, y = height * (1 + 2 * (k / 3)) / 6;
            next.samplePixels[k] = {i32(x), i32(y)};
            copy(ids, x, y, 1, 1, 2 * kProbePlane + k * 256);
            copy(depthCopy, x, y, 1, 1, 2 * kProbePlane + (kProbeSamples + k) * 256);
        }
        pendingProbes.push_back({readback, std::move(next)});
        return readback;
    }
    std::vector<std::pair<Readback*, Probe>> pendingProbes;

    void mapProbe(Readback& readback) {
        readback.buffer.MapAsync(
            wgpu::MapMode::Read, 0, usize(kProbeBytes), kCallbacks,
            [this, &readback, alive = std::weak_ptr<int>(lifetime)](wgpu::MapAsyncStatus status,
                                                                     wgpu::StringView) {
                if (alive.expired()) {
                    return;
                }
                const auto found = std::find_if(pendingProbes.begin(), pendingProbes.end(),
                                                [&](const auto& p) { return p.first == &readback; });
                if (status == wgpu::MapAsyncStatus::Success && found != pendingProbes.end()) {
                    Probe& next = found->second;
                    const auto* bytes = static_cast<const u8*>(
                        readback.buffer.GetConstMappedRange(0, usize(kProbeBytes)));
                    next.ids.resize(usize(kProbeSize) * next.height);
                    next.depths.resize(next.ids.size());
                    std::memcpy(next.ids.data(), bytes, next.ids.size() * 4);
                    std::memcpy(next.depths.data(), bytes + kProbePlane, next.depths.size() * 4);
                    for (u32 k = 0; k < kProbeSamples; ++k) {
                        std::memcpy(&next.sampleIds[k], bytes + 2 * kProbePlane + k * 256, 4);
                        std::memcpy(&next.sampleDepths[k],
                                    bytes + 2 * kProbePlane + (kProbeSamples + k) * 256, 4);
                    }
                    probe = std::move(next);
                    readback.buffer.Unmap();
                    ++probesArrived;
                } else {
                    ++probesFailed;
                }
                if (found != pendingProbes.end()) {
                    pendingProbes.erase(found);
                }
                readback.busy = false;
            });
    }

    void mapTiming(Readback& readback) {
        readback.buffer.MapAsync(
            wgpu::MapMode::Read, 0, 32, kCallbacks,
            [this, &readback, alive = std::weak_ptr<int>(lifetime)](wgpu::MapAsyncStatus status,
                                                                     wgpu::StringView) {
                if (alive.expired()) {
                    return;
                }
                if (status == wgpu::MapAsyncStatus::Success) {
                    u64 ticks[4];
                    std::memcpy(ticks, readback.buffer.GetConstMappedRange(0, 32), 32);
                    if (ticks[3] > ticks[0]) {
                        gpuMilliseconds = double(ticks[3] - ticks[0]) * 1e-6;
                        ++timedFrames;
                    }
                    readback.buffer.Unmap();
                }
                readback.busy = false;
            });
    }

    // ---- Classifying the shell -------------------------------------------------------------

    u32 classifySize = 0;
    wgpu::Texture classifyFrontIds, classifyBehindIds, classifyFrontDepths, classifyScratch;
    wgpu::Texture classifyDepth;
    wgpu::BindGroup peelGroup, frontGroup; // the front pass binds stand-ins for what it writes

    void ensureClassify(u32 size) {
        if (size == classifySize && peelGroup) {
            return;
        }
        classifySize = size;
        const Usage target = Usage::RenderAttachment;
        classifyFrontIds = texture(size, size, kIdFormat,
                                   target | Usage::TextureBinding | Usage::CopySrc, "front ids");
        classifyBehindIds = texture(size, size, kIdFormat, target | Usage::CopySrc, "behind ids");
        classifyFrontDepths =
            texture(size, size, kDepthCopyFormat, target | Usage::TextureBinding, "front depths");
        classifyScratch = texture(size, size, kDepthCopyFormat, target, "behind depths");
        classifyDepth = texture(size, size, kDepthFormat, target, "classify depth");
        peelGroup = makeGroup(device, peelLayout, {bufferEntry(0, slots),
                                                   textureEntry(1, classifyFrontIds),
                                                   textureEntry(2, classifyFrontDepths)});
        frontGroup = makeGroup(
            device, peelLayout,
            {bufferEntry(0, slots),
             textureEntry(1, texture(1, 1, kIdFormat, Usage::TextureBinding, "no ids")),
             textureEntry(2, texture(1, 1, kDepthCopyFormat, Usage::TextureBinding, "no depths"))});
    }

    // Each pixel's nearest occurrence, and the nearest other occurrence behind it, in a square
    // view: the front pass, then a peel that keeps only what lies behind the front, as Metal's.
    render::OccurrenceIds classify(const render::ViewState& view, u32 size) {
        render::OccurrenceIds result;
        if (surfaceInstances == 0 || size == 0) {
            return result;
        }
#ifdef __EMSCRIPTEN__
        // The browser cannot wait for the GPU; the web runtime asks for these asynchronously.
        (void)view;
        return result;
#else
        ensureGroups();
        ensureClassify(size);
        writeFrame(kClassifySlot, view.uniforms, nullptr);
        resetCounters(false);
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        encodeCull(encoder, false, kClassifySlot);
        for (const bool peel : {false, true}) {
            wgpu::RenderPassColorAttachment attachments[3] = {};
            const wgpu::Texture* textures[2] = {peel ? &classifyBehindIds : &classifyFrontIds,
                                                peel ? &classifyScratch : &classifyFrontDepths};
            for (u32 t = 0; t < 2; ++t) {
                attachments[t + 1].view = textures[t]->CreateView();
                attachments[t + 1].loadOp = wgpu::LoadOp::Clear;
                attachments[t + 1].storeOp = wgpu::StoreOp::Store;
                attachments[t + 1].clearValue = {0, 0, 0, 0};
            }
            wgpu::RenderPassDepthStencilAttachment depthAttachment{};
            depthAttachment.view = classifyDepth.CreateView();
            depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
            depthAttachment.depthStoreOp = wgpu::StoreOp::Discard;
            depthAttachment.depthClearValue = 0.0f;
            wgpu::RenderPassDescriptor descriptor{};
            descriptor.colorAttachmentCount = 3;
            descriptor.colorAttachments = attachments;
            descriptor.depthStencilAttachment = &depthAttachment;
            wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&descriptor);
            const u32 offset = u32(kClassifySlot * kUniformStride);
            pass.SetBindGroup(0, sceneGroup, 1, &offset);
            pass.SetBindGroup(1, peel ? peelGroup : frontGroup);
            pass.SetIndexBuffer(indices, wgpu::IndexFormat::Uint16);
            drawPair(pass, {.kind = Kind::Classify, .peel = peel}, 0);
            pass.End();
        }
        const u32 stride = (size * 4 + 255) & ~255u;
        const u64 plane = u64(stride) * size;
        wgpu::Buffer readback = buffer(plane * 2, U::MapRead | U::CopyDst, "classify readback");
        for (u32 t = 0; t < 2; ++t) {
            wgpu::TexelCopyTextureInfo source{};
            source.texture = t == 0 ? classifyFrontIds : classifyBehindIds;
            wgpu::TexelCopyBufferInfo destination{};
            destination.buffer = readback;
            destination.layout.offset = t * plane;
            destination.layout.bytesPerRow = stride;
            destination.layout.rowsPerImage = size;
            const wgpu::Extent3D extent = {size, size, 1};
            encoder.CopyTextureToBuffer(&source, &destination, &extent);
        }
        const wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        wait(readback.MapAsync(wgpu::MapMode::Read, 0, usize(plane * 2),
                               wgpu::CallbackMode::WaitAnyOnly,
                               [](wgpu::MapAsyncStatus, wgpu::StringView) {}));
        const auto* bytes =
            static_cast<const u8*>(readback.GetConstMappedRange(0, usize(plane * 2)));
        result.front.resize(usize(size) * size);
        result.behind.resize(usize(size) * size);
        for (u32 y = 0; y < size; ++y) {
            std::memcpy(result.front.data() + usize(y) * size, bytes + usize(y) * stride, size * 4);
            std::memcpy(result.behind.data() + usize(y) * size, bytes + plane + usize(y) * stride,
                        size * 4);
        }
        readback.Unmap();
        return result;
#endif
    }

    // Blocks until `future` completes. Only native hosts may.
    void wait(const wgpu::Future& future) {
        instance.WaitAny(future, UINT64_MAX);
    }
};

Renderer::Renderer() : state_(std::make_unique<State>()) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::create(wgpu::Instance instance, wgpu::Device device,
                                           wgpu::TextureFormat outputFormat, std::string& error) {
    std::unique_ptr<Renderer> renderer(new Renderer);
    State& s = *renderer->state_;
    s.instance = instance;
    s.device = device;
    s.queue = device.GetQueue();
    s.outputFormat = outputFormat;
    device.GetLimits(&s.limits);
    if (!s.createModules()) {
        error = "The WebGPU shaders could not be compiled.";
        return nullptr;
    }
    s.createLayouts();
    s.createFixedPipelines();
    if (device.HasFeature(wgpu::FeatureName::TimestampQuery)) {
        wgpu::QuerySetDescriptor queries{};
        queries.type = wgpu::QueryType::Timestamp;
        queries.count = 4;
        s.timestamps = device.CreateQuerySet(&queries);
        s.timestampResolve = s.buffer(32, U::QueryResolve | U::CopySrc, "timestamps");
    }
    s.style =
        render::makeStyleUniforms(render::styleNamed(render::StyleName::Illustration), {}, 2.0f);
    return renderer;
}

void Renderer::syncScene(const scene::Scene& scene) { state_->syncScene(scene); }

void Renderer::syncState(const model::FaceState& state) {
    State& s = *state_;
    if (state.version() == s.stateVersion) {
        return;
    }
    s.stateVersion = state.version();
    const std::span<const OccurrenceState> words = state.occurrenceWords();
    s.words.assign(words.begin(), words.end());
    s.words.resize(s.occurrenceInfo.size(), OccurrenceState{0, 0});
    const std::span<const u8> faces = state.faceBytes();
    std::vector<u8> padded(std::max<usize>((faces.size() + 3) & ~usize(3), 4), 0);
    std::copy(faces.begin(), faces.end(), padded.begin());
    if (!s.faceStates || s.faceStates.GetSize() < padded.size()) {
        s.faceStates = s.buffer(padded.size(), U::Storage | U::CopyDst, "face states");
    }
    s.queue.WriteBuffer(s.faceStates, 0, padded.data(), padded.size());
    s.uploadStates();
    s.planLists();
}

void Renderer::setStyle(const StyleUniforms& style, bool physical) {
    State& s = *state_;
    const bool shadowChanged = (style.shadowOn != 0) != (s.style.shadowOn != 0);
    s.style = style;
    s.physical = physical;
    if (shadowChanged) {
        s.planLists();
    }
}

void Renderer::syncLooks(std::span<const u16> lookOfOccurrence, std::span<const u32> palette) {
    State& s = *state_;
    s.looks.assign(lookOfOccurrence.begin(), lookOfOccurrence.end());
    s.looks.resize(s.occurrenceInfo.size(), 0);
    s.palette = s.buffer(std::max<usize>(palette.size_bytes(), 12), U::Storage | U::CopyDst,
                         "palette");
    s.queue.WriteBuffer(s.palette, 0, palette.data(), palette.size_bytes());
    s.uploadStates();
    s.planLists();
}

void Renderer::setReducedDetail(bool reduced) { state_->reducedDetail = reduced; }
u64 Renderer::sceneTriangles() const { return state_->triangles; }
u64 Renderer::sceneEdgeSegments() const { return state_->edgeSegments; }
void Renderer::resize(u32 width, u32 height) { state_->resize(width, height); }
float Renderer::glyphAdvance() const { return state_->glyphAdvance; }

void Renderer::setOverlayAtlas(std::span<const u8> pixels, float glyphAdvance) {
    State& s = *state_;
    if (pixels.size() < usize(kAtlasWidth) * kAtlasHeight) {
        return;
    }
    s.glyphAdvance = glyphAdvance;
    s.atlas = s.texture(kAtlasWidth, kAtlasHeight, Format::R8Unorm,
                        Usage::TextureBinding | Usage::CopyDst, "overlay atlas");
    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = s.atlas;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = kAtlasWidth;
    layout.rowsPerImage = kAtlasHeight;
    const wgpu::Extent3D extent = {kAtlasWidth, kAtlasHeight, 1};
    s.queue.WriteTexture(&destination, pixels.data(), usize(kAtlasWidth) * kAtlasHeight, &layout,
                         &extent);
    s.overlayUniforms = s.buffer(sizeof(OverlayUniforms), U::Uniform | U::CopyDst, "overlay");
    s.overlayQuads = s.buffer(sizeof(OverlayQuad) * kOverlayMaxQuads, U::Storage | U::CopyDst,
                              "overlay quads");
    wgpu::BindGroupEntry sampler{};
    sampler.binding = 3;
    sampler.sampler = s.linearClamp;
    s.overlayGroup = makeGroup(s.device, s.overlayLayout,
                               {bufferEntry(0, s.overlayUniforms), bufferEntry(1, s.overlayQuads),
                                textureEntry(2, s.atlas), sampler});
}
void Renderer::setSamples(u32 samples) { state_->samples = std::clamp(samples, 1u, 4u); }

// From the newest probe that has come back: what the frame before last or so drew. Asking also
// moves the next frames' window here, so a pointer that stops is answered exactly.
std::optional<render::PixelHit> Renderer::readPixel(int x, int y,
                                                    const render::DepthDecoder& depth) const {
    const State& s = *state_;
    s.probeX = x;
    s.probeY = y;
    const State::Probe& p = s.probe;
    u32 id = 0;
    float value = 0.0f;
    bool found = false;
    if (x >= p.x0 && y >= p.y0 && x < p.x0 + i32(p.width) && y < p.y0 + i32(p.height) &&
        !p.ids.empty()) {
        const usize at = usize(y - p.y0) * State::kProbeSize + usize(x - p.x0);
        id = p.ids[at];
        value = p.depths[at];
        found = true;
    }
    for (u32 k = 0; k < State::kProbeSamples && !found; ++k) {
        if (p.samplePixels[k][0] == x && p.samplePixels[k][1] == y) {
            id = p.sampleIds[k];
            value = p.sampleDepths[k];
            found = true;
        }
    }
    if (!found || id == 0) {
        return std::nullopt;
    }
    return render::PixelHit{id, depth.viewDepth(value)};
}

double Renderer::gpuMilliseconds() const { return state_->gpuMilliseconds; }

std::array<u64, 2> Renderer::memoryBytes() const {
    const State& s = *state_;
    u64 buffers = 0, targets = 0;
    for (const wgpu::Buffer* b :
         {&s.frames, &s.styleBuffer, &s.definitions, &s.occurrences, &s.geometry, &s.instances,
          &s.states, &s.faceStates, &s.palette, &s.lists, &s.counters, &s.slots, &s.indices}) {
        buffers += *b ? b->GetSize() : 0;
    }
    const auto texel = [](wgpu::TextureFormat format) -> u64 {
        switch (format) {
        case Format::R8Unorm:
            return 1;
        case Format::RG8Uint:
            return 2;
        case Format::RGBA16Float:
            return 8;
        default:
            return 4;
        }
    };
    for (const wgpu::Texture* t : {&s.color, &s.ids, &s.depthCopy, &s.ink, &s.tags, &s.depth,
                                   &s.shadowMask, &s.occlusionTexture, &s.accum, &s.offscreen}) {
        if (*t) {
            targets += u64(t->GetWidth()) * t->GetHeight() * texel(t->GetFormat());
        }
    }
    return {buffers, targets};
}
std::array<u64, 2> Renderer::probeCounts() const {
    return {state_->probesArrived, state_->probesFailed};
}
std::array<u64, 2> Renderer::probeContents() const {
    const State::Probe& p = state_->probe;
    u64 ids = 0, depths = 0;
    for (usize i = 0; i < p.ids.size(); ++i) {
        ids += p.ids[i] != 0 ? 1 : 0;
        depths += p.depths[i] != 0.0f ? 1 : 0;
    }
    return {ids, depths};
}
u64 Renderer::timedFrames() const { return state_->timedFrames; }

render::OccurrenceIds Renderer::renderOccurrenceIds(const render::ViewState& view, u32 size) {
    return state_->classify(view, size);
}

void Renderer::render(const wgpu::Texture& target, const render::ViewState& view,
                      const render::Overlay* overlay) {
    state_->encodeFrame(target, view, state_->samples, overlay);
}

std::vector<u8> Renderer::renderOffscreen(const render::ViewState& view,
                                          const render::Overlay* overlay) {
    State& s = *state_;
    s.encodeFrame(s.offscreen, view, 4, overlay);
    const u32 stride = (s.width * 4 + 255) & ~255u;
    wgpu::Buffer readback = s.buffer(u64(stride) * s.height, U::MapRead | U::CopyDst, "readback");
    wgpu::TexelCopyTextureInfo source{};
    source.texture = s.offscreen;
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = readback;
    destination.layout.bytesPerRow = stride;
    const wgpu::Extent3D extent = {s.width, s.height, 1};
    wgpu::CommandEncoder encoder = s.device.CreateCommandEncoder();
    encoder.CopyTextureToBuffer(&source, &destination, &extent);
    const wgpu::CommandBuffer commands = encoder.Finish();
    s.queue.Submit(1, &commands);
    const usize size = usize(readback.GetSize());
    s.wait(readback.MapAsync(wgpu::MapMode::Read, 0, size,
                             wgpu::CallbackMode::WaitAnyOnly,
                             [](wgpu::MapAsyncStatus, wgpu::StringView) {}));
    const auto* bytes = static_cast<const u8*>(readback.GetConstMappedRange(0, size));
    std::vector<u8> pixels(usize(s.width) * s.height * 4);
    const bool swap = s.outputFormat == Format::RGBA8Unorm;
    for (u32 y = 0; y < s.height; ++y) {
        for (u32 x = 0; x < s.width; ++x) {
            const u8* from = bytes + usize(y) * stride + usize(x) * 4;
            u8* to = pixels.data() + (usize(y) * s.width + x) * 4;
            to[0] = swap ? from[2] : from[0];
            to[1] = from[1];
            to[2] = swap ? from[0] : from[2];
            to[3] = from[3];
        }
    }
    readback.Unmap();
    return pixels;
}

} // namespace geo::webgpu
