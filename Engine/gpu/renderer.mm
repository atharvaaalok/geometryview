#include "gpu/renderer.h"

#include "base/assert.h"
#include "base/trace.h"
#include "gpu/gpu_context.h"
#include "gpu/overlay_atlas.h"
#include "gpu/pipelines.h"
#include "gpu/scene_tables.h"
#include "gpu/targets.h"
#include "render/style.h"
#include "shaders/shared/gpu_types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace geo::gpu {
namespace {

// With two drawables, a third frame cannot start until an earlier one finishes, so two allocators
// are enough: reusing one only waits for the frame that last used it.
constexpr usize kFramesInFlight = 2;
constexpr u64 kFrameTimeoutMs = 2000;

void reportFailure(const char* what, NSError* error) {
    std::fprintf(stderr, "%s: %s\n", what,
                 error ? error.localizedDescription.UTF8String : "unknown error");
}

// A small buffer with one slot per frame in flight, written by the CPU each frame.
template <typename T> struct FrameRing {
    id<MTLBuffer> buffer;
    usize count = 1; // records per slot

    void create(const GpuContext& context, usize records = 1) {
        count = records;
        buffer = context.makeShared(sizeof(T) * count * kFramesInFlight);
        [context.residency addAllocation:buffer];
    }
    // Copies `n` records into this frame's slot and returns its GPU address.
    u64 write(u64 frame, const T* records, usize n = 1) {
        const usize offset = (frame % kFramesInFlight) * sizeof(T) * count;
        std::memcpy(static_cast<u8*>(buffer.contents) + offset, records, sizeof(T) * n);
        return buffer.gpuAddress + offset;
    }
};

} // namespace

struct Renderer::State {
    GpuContext context;
    Retirement retirement;
    Pipelines pipelines;
    Targets targets;
    SceneTables tables;
    OverlayAtlas atlas;

    id<MTLLibrary> library;
    id<MTL4CommandBuffer> commandBuffer;
    std::array<id<MTL4CommandAllocator>, kFramesInFlight> allocators;
    id<MTL4ArgumentTable> surfaceTable, tileTable, overlayTable, compositeTable;

    FrameRing<FrameUniforms> frameUniforms;
    FrameRing<StyleUniforms> styleUniforms;
    FrameRing<OverlayUniforms> overlayUniforms;
    FrameRing<OverlayQuad> overlayQuads;

    StyleUniforms style{};
    bool physical = false;
    bool section = false; // this frame has section planes

    // Hidden, ghosted and selected flags. Uploaded only when they change.
    id<MTLBuffer> occurrenceStates, faceStates;
    u64 stateVersion = ~u64(0);
    bool hasGhosts = false;

    // Each occurrence's look, and three palette texels per look.
    id<MTLBuffer> looks, palette;
    bool hasShells = false; // some occurrence's look marks it as shell

    // The square views that classify the shell, with their own targets and argument table.
    id<MTL4ArgumentTable> classifyTable;
    u32 classifySize = 0;
    id<MTLTexture> classifyFrontIds, classifyBehindIds, classifyFrontDepths;
    id<MTLTexture> classifyColor, classifyDepthCopy, classifyInk, classifyTags, classifyDepth;

    // GPU time, from commit feedback while tracing.
    std::atomic<double> gpuSeconds{0.0};
    std::atomic<u64> gpuFrames{0};

    // GEOMETRY_NO_EDGES leaves feature edges out, to measure what they cost.
    bool drawsEdges = std::getenv("GEOMETRY_NO_EDGES") == nullptr;

    bool createTables() {
        NSError* error = nil;
        MTL4ArgumentTableDescriptor* surface = [MTL4ArgumentTableDescriptor new];
        surface.maxBufferBindCount = kBufferCount;
        surfaceTable = [context.device newArgumentTableWithDescriptor:surface error:&error];

        MTL4ArgumentTableDescriptor* tile = [MTL4ArgumentTableDescriptor new];
        tile.maxTextureBindCount = 3;
        tile.maxBufferBindCount = kBufferCount;
        tileTable = [context.device newArgumentTableWithDescriptor:tile error:&error];

        MTL4ArgumentTableDescriptor* composite = [MTL4ArgumentTableDescriptor new];
        composite.maxBufferBindCount = kBufferCount;
        composite.maxTextureBindCount = 5;
        compositeTable = [context.device newArgumentTableWithDescriptor:composite error:&error];

        MTL4ArgumentTableDescriptor* overlay = [MTL4ArgumentTableDescriptor new];
        overlay.maxBufferBindCount = 2;
        overlay.maxTextureBindCount = 1;
        overlayTable = [context.device newArgumentTableWithDescriptor:overlay error:&error];
        MTL4ArgumentTableDescriptor* classify = [MTL4ArgumentTableDescriptor new];
        classify.maxBufferBindCount = kBufferCount;
        classify.maxTextureBindCount = 2;
        classifyTable = [context.device newArgumentTableWithDescriptor:classify error:&error];
        if (!surfaceTable || !tileTable || !overlayTable || !compositeTable || !classifyTable) {
            reportFailure("Could not create argument tables", error);
            return false;
        }

        MTLResidencySetDescriptor* set = [MTLResidencySetDescriptor new];
        context.residency = [context.device newResidencySetWithDescriptor:set error:&error];
        if (!context.residency) {
            reportFailure("Could not create a residency set", error);
            return false;
        }
        [context.queue addResidencySet:context.residency];
        return true;
    }

    // Replaces a buffer with one holding `bytes`, retiring the old one once no frame reads it.
    void replace(id<MTLBuffer> __strong& buffer, const void* bytes, usize size) {
        id<MTLBuffer> fresh = context.makeShared(size);
        if (size != 0) {
            std::memcpy(fresh.contents, bytes, size);
        }
        if (buffer != nil) {
            retirement.retire(context.frameCount, {buffer});
        }
        buffer = fresh;
        [context.residency addAllocation:buffer];
        [context.residency commit];
    }

    void encodeOverlay(id<MTL4RenderCommandEncoder> encoder, const render::Overlay& overlay) {
        const u64 frame = context.frameCount;
        [overlayTable setAddress:overlayUniforms.write(frame, &overlay.uniforms)
                         atIndex:kOverlayUniforms];
        [overlayTable
            setAddress:overlayQuads.write(frame, overlay.quads.data(), overlay.uniforms.quadCount)
               atIndex:kOverlayQuads];
        [encoder setArgumentTable:overlayTable
                         atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [encoder setDepthStencilState:pipelines.depthAlways];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setRenderPipelineState:pipelines.cube];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
        if (overlay.uniforms.quadCount != 0) {
            [encoder setRenderPipelineState:pipelines.quad];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:6
                      instanceCount:overlay.uniforms.quadCount];
        }
    }

    // Draws up to kMaxBatchesPerDraw batches at a time.
    void drawBatches(id<MTL4RenderCommandEncoder> encoder, BatchRange range,
                     NSUInteger meshThreads = kMeshletMaxTriangles,
                     id<MTL4ArgumentTable> table = nil) {
        table = table != nil ? table : surfaceTable;
        for (usize start = range.first; start < range.last; start += kMaxBatchesPerDraw) {
            const usize groups = std::min<usize>(kMaxBatchesPerDraw, range.last - start);
            [table
                setAddress:tables.drawBases().gpuAddress + start / kMaxBatchesPerDraw * sizeof(u32)
                   atIndex:kBufferDrawBase];
            [encoder setArgumentTable:table
                             atStages:MTLRenderStageObject | MTLRenderStageMesh |
                                      MTLRenderStageFragment];
            [encoder drawMeshThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                  threadsPerMeshThreadgroup:MTLSizeMake(meshThreads, 1, 1)];
        }
    }

    // Edges into the ink attachment. The tile resolve inks the colour from it.
    void drawEdges(id<MTL4RenderCommandEncoder> encoder, bool ghostLines) {
        [surfaceTable setAddress:tables.edgeBatches().gpuAddress atIndex:kBufferBatches];
        [surfaceTable setAddress:tables.edgeRefs().gpuAddress atIndex:kBufferMeshletRefs];
        [encoder setRenderPipelineState:pipelines.edges[ghostLines][section]];
        [encoder setDepthStencilState:pipelines.depthTest];
        [encoder setCullMode:MTLCullModeNone];
        drawBatches(encoder, tables.allEdgeBatches(), kEdgeChunkSegments);
        [surfaceTable setAddress:tables.batches().gpuAddress atIndex:kBufferBatches];
        [surfaceTable setAddress:tables.meshletRefs().gpuAddress atIndex:kBufferMeshletRefs];
    }

    // Closed surfaces cull back faces, except while a section is open: the back faces seen
    // through a cut are its caps.
    void drawSurfaces(id<MTL4RenderCommandEncoder> encoder) {
        [encoder setCullMode:section ? MTLCullModeNone : MTLCullModeBack];
        drawBatches(encoder, tables.closedBatches());
        [encoder setCullMode:MTLCullModeNone];
        drawBatches(encoder, tables.openBatches());
    }

    // The main pass: opaque surfaces, the sheet behind them, creases and highlights, ghosts over
    // all of it with their dashed lines, then the overlay.
    // It ends by copying sample 0 of the ids and depths out of tile memory for picking.
    // Points every table at this frame's uniforms and the current scene.
    void bindFrame(const render::ViewState& view) {
        section = view.uniforms.sectionPlaneCount > 0;
        const u64 frame = context.frameCount;
        const u64 frameAddress = frameUniforms.write(frame, &view.uniforms);
        const u64 styleAddress = styleUniforms.write(frame, &style);
        for (id<MTL4ArgumentTable> table :
             {surfaceTable, compositeTable, tileTable, classifyTable}) {
            [table setAddress:frameAddress atIndex:kBufferFrame];
            [table setAddress:styleAddress atIndex:kBufferStyle];
        }
        if (!tables.empty()) {
            for (id<MTL4ArgumentTable> table : {surfaceTable, classifyTable}) {
                [table setAddress:tables.definitions().gpuAddress atIndex:kBufferDefinitions];
                [table setAddress:tables.occurrences().gpuAddress atIndex:kBufferOccurrences];
                [table setAddress:tables.batches().gpuAddress atIndex:kBufferBatches];
                [table setAddress:tables.meshletRefs().gpuAddress atIndex:kBufferMeshletRefs];
                [table setAddress:occurrenceStates.gpuAddress atIndex:kBufferOccurrenceState];
                [table setAddress:faceStates.gpuAddress atIndex:kBufferFaceState];
                [table setAddress:palette.gpuAddress atIndex:kBufferPalette];
                [table setAddress:looks.gpuAddress atIndex:kBufferLooks];
            }
        }
    }

    bool castsShadow(const render::ViewState& view) const {
        return !tables.empty() && style.shadowOn != 0 && view.uniforms.shadowVisibility > 0.0f;
    }

    // Solid surfaces flattened onto the ground, into the shadow mask the sheet reads.
    void encodeShadow() {
        id<MTL4RenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:targets.shadowPass()];
        // The previous frame's sheet may still read the mask.
        [encoder barrierAfterQueueStages:MTLStageFragment
                            beforeStages:MTLStageFragment
                       visibilityOptions:MTL4VisibilityOptionNone];
        [encoder setRenderPipelineState:pipelines.shadow[section]];
        [encoder setCullMode:MTLCullModeNone];
        drawBatches(encoder, tables.closedBatches());
        drawBatches(encoder, tables.openBatches());
        [encoder endEncoding];
    }

    void encodeMain(id<MTL4RenderCommandEncoder> encoder, const render::ViewState& view,
                    const render::Overlay* overlay) {
        // The previous frame's composite may still read what this pass's tiles write, and the
        // sheet reads the shadow mask just drawn.
        [encoder barrierAfterQueueStages:MTLStageFragment
                            beforeStages:MTLStageTile | MTLStageFragment
                       visibilityOptions:MTL4VisibilityOptionDevice];
        [encoder setFrontFacingWinding:MTLWindingCounterClockwise];

        const bool hasScene = !tables.empty();
        if (hasScene) {
            [encoder setRenderPipelineState:pipelines.surface[physical][section]];
            [encoder setDepthStencilState:pipelines.depthWrite];
            drawSurfaces(encoder);
        }
        const bool seeThrough = hasScene && hasShells && view.uniforms.seeThrough > 0.0f;
        if (seeThrough) {
            // The shell's far walls: back faces, held just in front of the sheet by a depth
            // range of their own, so they always lie under the mechanism.
            [encoder setRenderPipelineState:pipelines.shellBack[physical][section]];
            [encoder setDepthStencilState:pipelines.depthWrite];
            [encoder setViewport:MTLViewport{0, 0, double(targets.width), double(targets.height),
                                             1e-6, 1e-4}];
            [encoder setCullMode:MTLCullModeFront];
            drawBatches(encoder, tables.closedBatches());
            drawBatches(encoder, tables.openBatches());
            [encoder
                setViewport:MTLViewport{0, 0, double(targets.width), double(targets.height), 0, 1}];
        }

        // The sheet only where nothing was drawn, so hidden-surface removal skips it elsewhere.
        [encoder setRenderPipelineState:pipelines.sheet];
        [encoder setDepthStencilState:pipelines.farOnly];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setArgumentTable:compositeTable
                         atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (hasScene && drawsEdges) {
            drawEdges(encoder, false);
        }
        if (seeThrough) {
            // The shell's nearest front faces, over everything opaque: into depth, then blended
            // once per sample.
            [encoder setRenderPipelineState:pipelines.shellDepth[section]];
            [encoder setDepthStencilState:pipelines.depthWrite];
            [encoder setCullMode:MTLCullModeBack];
            drawBatches(encoder, tables.closedBatches());
            [encoder setCullMode:MTLCullModeNone];
            drawBatches(encoder, tables.openBatches());
            [encoder setRenderPipelineState:pipelines.shellFront[physical][section]];
            [encoder setDepthStencilState:pipelines.blendOnce];
            [encoder setStencilReferenceValue:0];
            [encoder setCullMode:MTLCullModeBack];
            drawBatches(encoder, tables.closedBatches());
            [encoder setCullMode:MTLCullModeNone];
            drawBatches(encoder, tables.openBatches());
        }

        // Ghosts last, blended over everything opaque and writing no depth.
        if (hasScene && hasGhosts) {
            [encoder setRenderPipelineState:pipelines.ghost[physical][view.uniforms.ghostPickable]
                                                           [section]];
            [encoder setDepthStencilState:pipelines.depthTest];
            drawSurfaces(encoder);
            if ((style.ghostStyle & kGhostLines) != 0 && drawsEdges) {
                drawEdges(encoder, true);
            }
        }

        if (overlay != nullptr && overlay->visible) {
            encodeOverlay(encoder, *overlay);
        }

        [encoder setRenderPipelineState:pipelines.resolveTile];
        [encoder setArgumentTable:tileTable atStages:MTLRenderStageTile];
        [encoder dispatchThreadsPerTile:MTLSizeMake(kTileSize, kTileSize, 1)];
    }

    void beginFrame() {
        // Reusing an allocator requires the frame that last used it to have finished.
        const u64 frame = ++context.frameCount;
        if (frame > kFramesInFlight) {
            const bool finished =
                [context.frameFinished waitUntilSignaledValue:frame - kFramesInFlight
                                                    timeoutMS:kFrameTimeoutMs];
            GEO_ASSERT(finished);
        }
        retirement.releaseFinished(context);
        id<MTL4CommandAllocator> allocator = allocators[frame % kFramesInFlight];
        [allocator reset];
        [commandBuffer beginCommandBufferWithAllocator:allocator];
    }

    void encodeFrame(id<MTLTexture> target, const render::ViewState& view,
                     const render::Overlay* overlay) {
        beginFrame();
        bindFrame(view);
        if (castsShadow(view)) {
            encodeShadow();
        }
        id<MTL4RenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:targets.mainPass(target)];
        encodeMain(encoder, view, overlay);
        [encoder endEncoding];

        if (style.ao > 0.0f) {
            // Reads the tiles' depth, and writes what the previous frame's composite read.
            id<MTL4ComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
            [compute barrierAfterQueueStages:MTLStageTile | MTLStageFragment
                                beforeStages:MTLStageDispatch
                           visibilityOptions:MTL4VisibilityOptionDevice];
            [compute setComputePipelineState:pipelines.ambientOcclusion];
            [compute setArgumentTable:compositeTable];
            [compute dispatchThreads:MTLSizeMake(targets.occlusion.width, targets.occlusion.height,
                                                 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [compute endEncoding];
        }

        id<MTL4RenderCommandEncoder> composite =
            [commandBuffer renderCommandEncoderWithDescriptor:targets.compositePass(target)];
        [composite barrierAfterQueueStages:MTLStageTile | MTLStageFragment | MTLStageDispatch
                              beforeStages:MTLStageFragment
                         visibilityOptions:MTL4VisibilityOptionDevice];
        [composite setRenderPipelineState:pipelines.composite];
        [composite setArgumentTable:compositeTable
                           atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [composite drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [composite endEncoding];
        [commandBuffer endCommandBuffer];
    }

    void commitAndSignal() {
        id<MTL4CommandBuffer> buffers[] = {commandBuffer};
        if (!traceEnabled()) {
            [context.queue commit:buffers count:1];
            return;
        }
        // Feedback allocates per frame, so it runs only when tracing.
        MTL4CommitOptions* options = [MTL4CommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            if (feedback.error) {
                std::fprintf(stderr, "GPU error: %s\n",
                             feedback.error.localizedDescription.UTF8String);
                return;
            }
            const double seconds = feedback.GPUEndTime - feedback.GPUStartTime;
            gpuSeconds.store(gpuSeconds.load(std::memory_order_relaxed) + seconds,
                             std::memory_order_relaxed);
            gpuFrames.fetch_add(1, std::memory_order_relaxed);
        }];
        [context.queue commit:buffers count:1 options:options];
    }
};

Renderer::Renderer() : state_(std::make_unique<State>()) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::create(id<MTLDevice> device, const char* libraryPath) {
    if (![device supportsFamily:MTLGPUFamilyMetal4]) {
        std::fprintf(stderr, "Geometry needs a GPU that supports Metal 4.\n");
        return nullptr;
    }
    std::unique_ptr<Renderer> renderer(new Renderer);
    State& s = *renderer->state_;
    s.context.device = device;

    NSError* error = nil;
    s.library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(libraryPath)] error:&error];
    if (!s.library) {
        reportFailure("Could not load the shader library", error);
        return nullptr;
    }

    s.context.queue = [device newMTL4CommandQueue];
    s.context.frameFinished = [device newSharedEvent];
    s.commandBuffer = [device newCommandBuffer];
    for (usize i = 0; i < kFramesInFlight; ++i) {
        s.allocators[i] = [device newCommandAllocator];
    }
    if (!s.pipelines.create(device, s.library) || !s.createTables()) {
        return nullptr;
    }
    if (!makeOverlayAtlas(device, s.atlas)) {
        std::fprintf(stderr, "Could not draw the overlay's text.\n");
        return nullptr;
    }
    [s.overlayTable setTexture:s.atlas.texture.gpuResourceID atIndex:0];
    [s.context.residency addAllocation:s.atlas.texture];

    s.style =
        render::makeStyleUniforms(render::styleNamed(render::StyleName::Illustration), {}, 2.0f);
    s.frameUniforms.create(s.context);
    s.styleUniforms.create(s.context);
    s.overlayUniforms.create(s.context);
    s.overlayQuads.create(s.context, kOverlayMaxQuads);
    s.occurrenceStates = s.context.makeShared(sizeof(OccurrenceState));
    s.faceStates = s.context.makeShared(16);
    [s.context.residency addAllocation:s.occurrenceStates];
    [s.context.residency addAllocation:s.faceStates];
    const u16 noLooks = 0;
    const std::array<u32, 3> neutral = {0xFFF3FAFBu, 0xFFD2E0E3u, 0xFFA6B8BDu};
    s.replace(s.looks, &noLooks, sizeof(noLooks));
    s.replace(s.palette, neutral.data(), sizeof(neutral));
    [s.context.residency commit];
    return renderer;
}

void Renderer::attachLayer(CAMetalLayer* layer) {
    [state_->context.queue addResidencySet:layer.residencySet];
}

void Renderer::syncScene(const scene::Scene& scene) {
    state_->tables.sync(state_->context, state_->retirement, scene);
}

void Renderer::setStyle(const StyleUniforms& style, bool physical) {
    state_->style = style;
    state_->physical = physical;
}

void Renderer::syncLooks(std::span<const u16> lookOfOccurrence, std::span<const u32> palette) {
    State& s = *state_;
    s.replace(s.looks, lookOfOccurrence.data(), lookOfOccurrence.size_bytes());
    s.replace(s.palette, palette.data(), palette.size_bytes());
    s.hasShells = std::any_of(lookOfOccurrence.begin(), lookOfOccurrence.end(),
                              [](u16 look) { return (look & kLookShell) != 0; });
}

OccurrenceIds Renderer::renderOccurrenceIds(const render::ViewState& view, u32 size) {
    State& s = *state_;
    OccurrenceIds result;
    if (s.tables.empty()) {
        return result;
    }
    id<MTLDevice> device = s.context.device;
    if (s.classifySize != size) {
        const auto make = [&](MTLPixelFormat format, MTLStorageMode storage,
                              MTLTextureUsage usage) {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                   width:size
                                                                  height:size
                                                               mipmapped:NO];
            d.storageMode = storage;
            d.usage = usage;
            id<MTLTexture> texture = [device newTextureWithDescriptor:d];
            if (storage != MTLStorageModeMemoryless) {
                [s.context.residency addAllocation:texture];
            }
            return texture;
        };
        const MTLTextureUsage target = MTLTextureUsageRenderTarget;
        const MTLTextureUsage read = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        s.classifyFrontIds = make(kIdFormat, MTLStorageModeShared, read);
        s.classifyBehindIds = make(kIdFormat, MTLStorageModeShared, target);
        s.classifyFrontDepths = make(kDepthCopyFormat, MTLStorageModePrivate, read);
        s.classifyColor = make(kColorFormat, MTLStorageModeMemoryless, target);
        s.classifyDepthCopy = make(kDepthCopyFormat, MTLStorageModeMemoryless, target);
        s.classifyInk = make(kInkFormat, MTLStorageModeMemoryless, target);
        s.classifyTags = make(kTagFormat, MTLStorageModeMemoryless, target);
        s.classifyDepth = make(kDepthFormat, MTLStorageModeMemoryless, target);
        [s.context.residency commit];
        [s.classifyTable setTexture:s.classifyFrontIds.gpuResourceID atIndex:kTexturePickIds];
        [s.classifyTable setTexture:s.classifyFrontDepths.gpuResourceID atIndex:kTexturePickDepths];
        s.classifySize = size;
    }

    s.beginFrame();
    s.bindFrame(view);
    for (const bool peel : {false, true}) {
        MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
        const auto attach = [&](NSUInteger index, id<MTLTexture> texture, bool store) {
            pass.colorAttachments[index].texture = texture;
            pass.colorAttachments[index].loadAction = MTLLoadActionClear;
            pass.colorAttachments[index].storeAction =
                store ? MTLStoreActionStore : MTLStoreActionDontCare;
            pass.colorAttachments[index].clearColor = MTLClearColorMake(0, 0, 0, 0);
        };
        attach(kAttachmentColor, s.classifyColor, false);
        attach(kAttachmentId, peel ? s.classifyBehindIds : s.classifyFrontIds, true);
        attach(kAttachmentDepth, peel ? s.classifyDepthCopy : s.classifyFrontDepths, !peel);
        attach(kAttachmentInk, s.classifyInk, false);
        attach(kAttachmentTags, s.classifyTags, false);
        pass.depthAttachment.texture = s.classifyDepth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.0;
        id<MTL4RenderCommandEncoder> encoder =
            [s.commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (peel) {
            [encoder barrierAfterQueueStages:MTLStageFragment
                                beforeStages:MTLStageFragment
                           visibilityOptions:MTL4VisibilityOptionDevice];
        }
        [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
        [encoder setRenderPipelineState:s.pipelines.classify[peel]];
        [encoder setDepthStencilState:s.pipelines.depthWrite];
        [encoder setCullMode:MTLCullModeBack];
        s.drawBatches(encoder, s.tables.closedBatches(), kMeshletMaxTriangles, s.classifyTable);
        [encoder setCullMode:MTLCullModeNone];
        s.drawBatches(encoder, s.tables.openBatches(), kMeshletMaxTriangles, s.classifyTable);
        [encoder endEncoding];
    }
    [s.commandBuffer endCommandBuffer];
    s.commitAndSignal();
    [s.context.queue signalEvent:s.context.frameFinished value:s.context.frameCount];
    const bool finished = [s.context.frameFinished waitUntilSignaledValue:s.context.frameCount
                                                                timeoutMS:kFrameTimeoutMs * 10];
    GEO_ASSERT(finished);

    result.front.resize(usize(size) * size);
    result.behind.resize(usize(size) * size);
    const MTLRegion region = MTLRegionMake2D(0, 0, size, size);
    [s.classifyFrontIds getBytes:result.front.data()
                     bytesPerRow:size * 4
                      fromRegion:region
                     mipmapLevel:0];
    [s.classifyBehindIds getBytes:result.behind.data()
                      bytesPerRow:size * 4
                       fromRegion:region
                      mipmapLevel:0];
    return result;
}

float Renderer::glyphAdvance() const { return state_->atlas.glyphAdvance; }

double Renderer::meanGpuSeconds() const {
    const u64 frames = state_->gpuFrames.load(std::memory_order_relaxed);
    return frames == 0 ? 0.0 : state_->gpuSeconds.load(std::memory_order_relaxed) / double(frames);
}

void Renderer::syncState(const model::FaceState& state) {
    State& s = *state_;
    if (state.version() == s.stateVersion) {
        return;
    }
    s.stateVersion = state.version();
    const std::span<const OccurrenceState> words = state.occurrenceWords();
    const std::span<const u8> faces = state.faceBytes();

    // Grown buffers replace the old ones, which a frame in flight may still read, so those retire.
    std::vector<id<MTLBuffer>> retired;
    const auto ensure = [&](id<MTLBuffer> __strong& buffer, usize bytes) {
        if (buffer.length >= bytes) {
            return;
        }
        retired.push_back(buffer);
        buffer = s.context.makeShared(std::max<usize>(bytes, buffer.length * 2));
        [s.context.residency addAllocation:buffer];
    };
    ensure(s.occurrenceStates, words.size_bytes());
    ensure(s.faceStates, faces.size_bytes());
    if (!retired.empty()) {
        s.retirement.retire(s.context.frameCount, std::move(retired));
        [s.context.residency commit];
    }
    std::memcpy(s.occurrenceStates.contents, words.data(), words.size_bytes());
    std::memcpy(s.faceStates.contents, faces.data(), faces.size_bytes());

    s.hasGhosts = std::any_of(words.begin(), words.end(), [](const OccurrenceState& word) {
        return (word.flags & (kStateGhost | kStateFaceGhost)) != 0;
    });
}

void Renderer::resize(u32 width, u32 height) {
    State& s = *state_;
    s.targets.resize(s.context, width, height);
    [s.tileTable setTexture:s.targets.pickIds.gpuResourceID atIndex:kTexturePickIds];
    [s.tileTable setTexture:s.targets.pickDepths.gpuResourceID atIndex:kTexturePickDepths];
    [s.tileTable setTexture:s.targets.tags.gpuResourceID atIndex:kTextureTags];
    [s.compositeTable setTexture:s.targets.pickDepths.gpuResourceID atIndex:kTexturePickDepths];
    [s.compositeTable setTexture:s.targets.tags.gpuResourceID atIndex:kTextureTags];
    [s.compositeTable setTexture:s.targets.occlusion.gpuResourceID atIndex:kTextureAo];
    [s.compositeTable setTexture:s.targets.shadowMask.gpuResourceID atIndex:kTextureShadow];
}

void Renderer::render(id<CAMetalDrawable> drawable, const render::ViewState& view,
                      const render::Overlay* overlay) {
    State& s = *state_;
    s.encodeFrame(drawable.texture, view, overlay);
    [s.context.queue waitForDrawable:drawable];
    s.commitAndSignal();
    [s.context.queue signalDrawable:drawable];
    [s.context.queue signalEvent:s.context.frameFinished value:s.context.frameCount];
    [drawable present];
}

std::vector<u8> Renderer::renderOffscreen(const render::ViewState& view,
                                          const render::Overlay* overlay) {
    State& s = *state_;
    s.encodeFrame(s.targets.offscreenColor, view, overlay);
    s.commitAndSignal();
    [s.context.queue signalEvent:s.context.frameFinished value:s.context.frameCount];
    const bool finished = [s.context.frameFinished waitUntilSignaledValue:s.context.frameCount
                                                                timeoutMS:kFrameTimeoutMs * 10];
    GEO_ASSERT(finished);

    const Targets& t = s.targets;
    std::vector<u8> pixels(usize(t.width) * t.height * 4);
    [t.offscreenColor getBytes:pixels.data()
                   bytesPerRow:NSUInteger(t.width) * 4
                    fromRegion:MTLRegionMake2D(0, 0, t.width, t.height)
                   mipmapLevel:0];
    return pixels;
}

std::optional<PixelHit> Renderer::readPixel(int x, int y, const render::DepthDecoder& depth) const {
    const Targets& t = state_->targets;
    if (x < 0 || y < 0 || u32(x) >= t.width || u32(y) >= t.height || !t.pickIdBuffer) {
        return std::nullopt;
    }
    const auto* ids = static_cast<const u8*>(t.pickIdBuffer.contents);
    const auto* depths = static_cast<const u8*>(t.pickDepthBuffer.contents);
    u32 id = 0;
    float value = 0.0f;
    std::memcpy(&id, ids + usize(y) * t.pickIdRowBytes + usize(x) * 4, sizeof(id));
    std::memcpy(&value, depths + usize(y) * t.pickDepthRowBytes + usize(x) * 4, sizeof(value));
    if (id == 0) {
        return std::nullopt;
    }
    return PixelHit{id, depth.viewDepth(value)};
}

} // namespace geo::gpu
