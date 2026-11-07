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
#include <cstdio>
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
    id<MTL4ArgumentTable> surfaceTable, tileTable, overlayTable;

    FrameRing<FrameUniforms> frameUniforms;
    FrameRing<StyleUniforms> styleUniforms;
    FrameRing<OverlayUniforms> overlayUniforms;
    FrameRing<OverlayQuad> overlayQuads;

    StyleUniforms style{};
    bool physical = false;

    // Hidden, ghosted and selected flags. Uploaded only when they change.
    id<MTLBuffer> occurrenceStates, faceStates;
    u64 stateVersion = ~u64(0);
    bool hasGhosts = false;

    // Each occurrence's look, and three palette texels per look.
    id<MTLBuffer> looks, palette;

    bool createTables() {
        NSError* error = nil;
        MTL4ArgumentTableDescriptor* surface = [MTL4ArgumentTableDescriptor new];
        surface.maxBufferBindCount = kBufferCount;
        surfaceTable = [context.device newArgumentTableWithDescriptor:surface error:&error];

        MTL4ArgumentTableDescriptor* tile = [MTL4ArgumentTableDescriptor new];
        tile.maxTextureBindCount = 2;
        tileTable = [context.device newArgumentTableWithDescriptor:tile error:&error];

        MTL4ArgumentTableDescriptor* overlay = [MTL4ArgumentTableDescriptor new];
        overlay.maxBufferBindCount = 2;
        overlay.maxTextureBindCount = 1;
        overlayTable = [context.device newArgumentTableWithDescriptor:overlay error:&error];
        if (!surfaceTable || !tileTable || !overlayTable) {
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
    void drawBatches(id<MTL4RenderCommandEncoder> encoder, BatchRange range) {
        for (usize start = range.first; start < range.last; start += kMaxBatchesPerDraw) {
            const usize groups = std::min<usize>(kMaxBatchesPerDraw, range.last - start);
            [surfaceTable
                setAddress:tables.drawBases().gpuAddress + start / kMaxBatchesPerDraw * sizeof(u32)
                   atIndex:kBufferDrawBase];
            [encoder setArgumentTable:surfaceTable
                             atStages:MTLRenderStageObject | MTLRenderStageMesh |
                                      MTLRenderStageFragment];
            [encoder drawMeshThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                  threadsPerMeshThreadgroup:MTLSizeMake(kMeshletMaxTriangles, 1, 1)];
        }
    }

    void drawSurfaces(id<MTL4RenderCommandEncoder> encoder) {
        [encoder setCullMode:MTLCullModeBack];
        drawBatches(encoder, tables.closedBatches());
        [encoder setCullMode:MTLCullModeNone];
        drawBatches(encoder, tables.openBatches());
    }

    // The main pass: opaque surfaces, the sheet behind them, ghosts over both, then the overlay.
    // It ends by copying sample 0 of the ids and depths out of tile memory for picking.
    void encodeMain(id<MTL4RenderCommandEncoder> encoder, const render::ViewState& view,
                    const render::Overlay* overlay) {
        const u64 frame = context.frameCount;
        [surfaceTable setAddress:frameUniforms.write(frame, &view.uniforms) atIndex:kBufferFrame];
        [surfaceTable setAddress:styleUniforms.write(frame, &style) atIndex:kBufferStyle];
        [encoder setFrontFacingWinding:MTLWindingCounterClockwise];

        const bool hasScene = !tables.empty();
        if (hasScene) {
            [surfaceTable setAddress:tables.definitions().gpuAddress atIndex:kBufferDefinitions];
            [surfaceTable setAddress:tables.occurrences().gpuAddress atIndex:kBufferOccurrences];
            [surfaceTable setAddress:tables.batches().gpuAddress atIndex:kBufferBatches];
            [surfaceTable setAddress:tables.meshletRefs().gpuAddress atIndex:kBufferMeshletRefs];
            [surfaceTable setAddress:occurrenceStates.gpuAddress atIndex:kBufferOccurrenceState];
            [surfaceTable setAddress:faceStates.gpuAddress atIndex:kBufferFaceState];
            [surfaceTable setAddress:palette.gpuAddress atIndex:kBufferPalette];
            [surfaceTable setAddress:looks.gpuAddress atIndex:kBufferLooks];
            [encoder setRenderPipelineState:pipelines.surface[physical]];
            [encoder setDepthStencilState:pipelines.depthWrite];
            drawSurfaces(encoder);
        }

        // The sheet only where nothing was drawn, so hidden-surface removal skips it elsewhere.
        [encoder setRenderPipelineState:pipelines.sheet];
        [encoder setDepthStencilState:pipelines.farOnly];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setArgumentTable:surfaceTable
                         atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // Ghosts last, blended over everything opaque and writing no depth.
        if (hasScene && hasGhosts) {
            [encoder setRenderPipelineState:pipelines.ghost[physical][view.uniforms.ghostPickable]];
            [encoder setDepthStencilState:pipelines.depthTest];
            drawSurfaces(encoder);
        }

        if (overlay != nullptr && overlay->visible) {
            encodeOverlay(encoder, *overlay);
        }

        [encoder setRenderPipelineState:pipelines.resolvePick];
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
        id<MTL4RenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:targets.mainPass(target)];
        encodeMain(encoder, view, overlay);
        [encoder endEncoding];
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
            }
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
}

float Renderer::glyphAdvance() const { return state_->atlas.glyphAdvance; }

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
