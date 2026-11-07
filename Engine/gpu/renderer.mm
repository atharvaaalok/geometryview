#include "gpu/renderer.h"

#include "base/assert.h"
#include "base/trace.h"
#include "shaders/shared/gpu_types.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace geo::gpu {
namespace {

// With two drawables, a third frame cannot start until an earlier one finishes, so two allocators
// are enough: reusing one only waits for the frame that last used it.
constexpr usize kFramesInFlight = 2;
constexpr u64 kFrameTimeoutMs = 2000;
constexpr NSUInteger kSampleCount = 4;
constexpr NSUInteger kPayloadBytes = sizeof(u32) * (kBatchMeshlets + 1);
// Definitions with fewer meshlets than this share packed batches with others.
constexpr u32 kPackedMeshletLimit = kBatchMeshlets / 2;

// Paper, #f2efe7, written as encoded sRGB because colours composite in encoded space.
const MTLClearColor kPaper = MTLClearColorMake(242.0 / 255.0, 239.0 / 255.0, 231.0 / 255.0, 1.0);

const MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
const MTLPixelFormat kIdFormat = MTLPixelFormatR32Uint;
const MTLPixelFormat kDepthCopyFormat = MTLPixelFormatR32Float;
const MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;

void reportFailure(const char* what, NSError* error) {
    std::fprintf(stderr, "%s: %s\n", what,
                 error ? error.localizedDescription.UTF8String : "unknown error");
}

} // namespace

struct Renderer::State {
    id<MTLDevice> device;
    id<MTLLibrary> library;
    id<MTL4CommandQueue> queue;
    id<MTL4CommandBuffer> commandBuffer;
    std::array<id<MTL4CommandAllocator>, kFramesInFlight> allocators;
    MTL4RenderPassDescriptor* pass = nil;
    id<MTLSharedEvent> frameFinished;
    u64 frameCount = 0;

    id<MTLRenderPipelineState> surfacePipeline;
    id<MTLRenderPipelineState> ghostPipeline;     // blended, writes no ids
    id<MTLRenderPipelineState> ghostPickPipeline; // blended, writes ids so a ghost can be clicked
    id<MTLRenderPipelineState> pickPipeline;
    id<MTLDepthStencilState> depthState;
    id<MTLDepthStencilState> ghostDepthState; // tests depth but does not write it
    id<MTL4ArgumentTable> surfaceTable;
    id<MTL4ArgumentTable> tileTable;
    id<MTLResidencySet> residency;

    id<MTLBuffer> frameUniforms;

    // Render targets. Everything multisampled is memoryless, so it costs no memory at all.
    u32 width = 0, height = 0;
    id<MTLTexture> colorMsaa, idMsaa, depthCopyMsaa, depthMsaa, offscreenColor;
    id<MTLBuffer> pickIdBuffer, pickDepthBuffer;
    id<MTLTexture> pickIds, pickDepths;
    NSUInteger pickIdRowBytes = 0, pickDepthRowBytes = 0;

    // Hidden, ghosted and selected flags. Uploaded only when they change.
    id<MTLBuffer> occurrenceStates, faceStates;
    u64 stateVersion = ~u64(0);
    bool hasGhosts = false;

    // Scene. Each segment of geometry has its own residency set, filled once and never written
    // again, so wiring its pages on another thread cannot race with anything.
    struct SegmentResidency {
        id<MTLBuffer> buffer;
        id<MTLResidencySet> set;
    };
    std::unordered_map<const scene::MappedSegment*, SegmentResidency> segmentBuffers;
    id<MTLBuffer> definitions, occurrences, batches, drawBases, meshletRefs;
    usize batchCount = 0;
    usize openBatchStart = 0;
    bool hasScene = false;

    // Buffers a frame may still read. They leave residency and memory once it has finished.
    struct Retired {
        u64 frame = 0;
        std::vector<id<MTLBuffer>> buffers;    // in the shared residency set
        std::vector<id<MTLResidencySet>> sets; // a segment's own set
    };
    std::vector<Retired> retired;

    void releaseFinished() {
        if (retired.empty()) {
            return;
        }
        const u64 finished = frameFinished.signaledValue;
        usize kept = 0;
        for (Retired& entry : retired) {
            if (entry.frame > finished) {
                retired[kept++] = std::move(entry);
                continue;
            }
            for (id<MTLBuffer> buffer : entry.buffers) {
                [residency removeAllocation:buffer];
            }
            for (id<MTLResidencySet> set : entry.sets) {
                [queue removeResidencySet:set];
            }
        }
        if (kept != retired.size()) {
            retired.resize(kept);
            [residency commit];
        }
    }

    id<MTLBuffer> makeShared(usize length) {
        return [device newBufferWithLength:std::max<usize>(length, 16)
                                   options:MTLResourceStorageModeShared |
                                           MTLResourceHazardTrackingModeUntracked];
    }

    id<MTLTexture> makeTarget(MTLPixelFormat format, NSUInteger samples, MTLStorageMode storage,
                              MTLTextureUsage usage) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        d.sampleCount = samples;
        d.storageMode = storage;
        d.usage = usage;
        return [device newTextureWithDescriptor:d];
    }

    MTL4LibraryFunctionDescriptor* function(NSString* name) {
        MTL4LibraryFunctionDescriptor* f = [MTL4LibraryFunctionDescriptor new];
        f.library = library;
        f.name = name;
        return f;
    }

    bool createPipelines() {
        NSError* error = nil;
        MTL4CompilerDescriptor* compilerDescriptor = [MTL4CompilerDescriptor new];
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:compilerDescriptor
                                                                error:&error];
        if (!compiler) {
            reportFailure("Could not create the shader compiler", error);
            return false;
        }

        surfacePipeline = makeSurfacePipeline(compiler, false, false, &error);
        ghostPipeline = makeSurfacePipeline(compiler, true, false, &error);
        ghostPickPipeline = makeSurfacePipeline(compiler, true, true, &error);
        if (!surfacePipeline || !ghostPipeline || !ghostPickPipeline) {
            reportFailure("Could not create the surface pipelines", error);
            return false;
        }

        MTL4TileRenderPipelineDescriptor* tile = [MTL4TileRenderPipelineDescriptor new];
        tile.tileFunctionDescriptor = function(@"resolvePick");
        tile.rasterSampleCount = kSampleCount;
        tile.threadgroupSizeMatchesTileSize = YES;
        tile.colorAttachments[kAttachmentColor].pixelFormat = kColorFormat;
        tile.colorAttachments[kAttachmentId].pixelFormat = kIdFormat;
        tile.colorAttachments[kAttachmentDepth].pixelFormat = kDepthCopyFormat;
        pickPipeline = [compiler newRenderPipelineStateWithDescriptor:tile
                                                  compilerTaskOptions:nil
                                                                error:&error];
        if (!pickPipeline) {
            reportFailure("Could not create the pick pipeline", error);
            return false;
        }

        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionGreater; // reversed depth
        depth.depthWriteEnabled = YES;
        depthState = [device newDepthStencilStateWithDescriptor:depth];
        depth.depthWriteEnabled = NO;
        ghostDepthState = [device newDepthStencilStateWithDescriptor:depth];
        return true;
    }

    // One function set serves both passes. The ghost pass is chosen by a function constant, so
    // each pipeline is specialised and the opaque one keeps no ghost branches.
    id<MTLRenderPipelineState> makeSurfacePipeline(id<MTL4Compiler> compiler, bool ghost,
                                                   bool ghostWritesIds, NSError** error) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&ghost type:MTLDataTypeBool atIndex:0];
        const auto specialized = [&](NSString* name) {
            MTL4SpecializedFunctionDescriptor* function = [MTL4SpecializedFunctionDescriptor new];
            function.functionDescriptor = this->function(name);
            function.constantValues = constants;
            return function;
        };
        MTL4MeshRenderPipelineDescriptor* mesh = [MTL4MeshRenderPipelineDescriptor new];
        mesh.objectFunctionDescriptor = specialized(@"surfaceObject");
        mesh.meshFunctionDescriptor = specialized(@"surfaceMesh");
        mesh.fragmentFunctionDescriptor = specialized(@"surfaceFragment");
        mesh.payloadMemoryLength = kPayloadBytes;
        mesh.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        mesh.rasterSampleCount = kSampleCount;
        mesh.colorAttachments[kAttachmentColor].pixelFormat = kColorFormat;
        mesh.colorAttachments[kAttachmentId].pixelFormat = kIdFormat;
        mesh.colorAttachments[kAttachmentDepth].pixelFormat = kDepthCopyFormat;
        if (ghost) {
            MTL4RenderPipelineColorAttachmentDescriptor* color =
                mesh.colorAttachments[kAttachmentColor];
            color.blendingState = MTL4BlendStateEnabled;
            color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            if (!ghostWritesIds) {
                mesh.colorAttachments[kAttachmentId].writeMask = MTLColorWriteMaskNone;
                mesh.colorAttachments[kAttachmentDepth].writeMask = MTLColorWriteMaskNone;
            }
        }
        return [compiler newRenderPipelineStateWithDescriptor:mesh
                                          compilerTaskOptions:nil
                                                        error:error];
    }

    bool createTables() {
        NSError* error = nil;
        MTL4ArgumentTableDescriptor* surface = [MTL4ArgumentTableDescriptor new];
        surface.maxBufferBindCount = 8;
        surfaceTable = [device newArgumentTableWithDescriptor:surface error:&error];

        MTL4ArgumentTableDescriptor* tile = [MTL4ArgumentTableDescriptor new];
        tile.maxTextureBindCount = 2;
        tileTable = [device newArgumentTableWithDescriptor:tile error:&error];
        if (!surfaceTable || !tileTable) {
            reportFailure("Could not create argument tables", error);
            return false;
        }

        MTLResidencySetDescriptor* set = [MTLResidencySetDescriptor new];
        residency = [device newResidencySetWithDescriptor:set error:&error];
        if (!residency) {
            reportFailure("Could not create a residency set", error);
            return false;
        }
        [queue addResidencySet:residency];
        return true;
    }

    // Built once per size. Each frame only changes the resolve target, so a frame allocates
    // nothing.
    MTL4RenderPassDescriptor* passDescriptor(id<MTLTexture> resolveTarget) {
        if (pass != nil) {
            pass.colorAttachments[kAttachmentColor].resolveTexture = resolveTarget;
            return pass;
        }
        pass = [MTL4RenderPassDescriptor new];
        pass.colorAttachments[kAttachmentColor].texture = colorMsaa;
        pass.colorAttachments[kAttachmentColor].resolveTexture = resolveTarget;
        pass.colorAttachments[kAttachmentColor].loadAction = MTLLoadActionClear;
        pass.colorAttachments[kAttachmentColor].storeAction = MTLStoreActionMultisampleResolve;
        pass.colorAttachments[kAttachmentColor].clearColor = kPaper;
        pass.colorAttachments[kAttachmentId].texture = idMsaa;
        pass.colorAttachments[kAttachmentId].loadAction = MTLLoadActionClear;
        pass.colorAttachments[kAttachmentId].storeAction = MTLStoreActionDontCare;
        pass.colorAttachments[kAttachmentId].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[kAttachmentDepth].texture = depthCopyMsaa;
        pass.colorAttachments[kAttachmentDepth].loadAction = MTLLoadActionClear;
        pass.colorAttachments[kAttachmentDepth].storeAction = MTLStoreActionDontCare;
        pass.colorAttachments[kAttachmentDepth].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.depthAttachment.texture = depthMsaa;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.0; // reversed depth
        pass.tileWidth = kTileSize;
        pass.tileHeight = kTileSize;
        return pass;
    }

    // Draws up to kMaxBatchesPerDraw batches at a time from [first, last).
    void drawBatches(id<MTL4RenderCommandEncoder> encoder, usize first, usize last) {
        for (usize start = first; start < last; start += kMaxBatchesPerDraw) {
            const usize groups = std::min<usize>(kMaxBatchesPerDraw, last - start);
            [surfaceTable setAddress:drawBases.gpuAddress + start / kMaxBatchesPerDraw * sizeof(u32)
                             atIndex:kBufferDrawBase];
            [encoder setArgumentTable:surfaceTable
                             atStages:MTLRenderStageObject | MTLRenderStageMesh |
                                      MTLRenderStageFragment];
            [encoder drawMeshThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                  threadsPerMeshThreadgroup:MTLSizeMake(kMeshletMaxTriangles, 1, 1)];
        }
    }

    void encodeScene(id<MTL4RenderCommandEncoder> encoder, const render::ViewState& view,
                     usize frame) {
        const usize uniformOffset = (frame % kFramesInFlight) * sizeof(FrameUniforms);
        std::memcpy(static_cast<u8*>(frameUniforms.contents) + uniformOffset, &view.uniforms,
                    sizeof(FrameUniforms));

        [surfaceTable setAddress:frameUniforms.gpuAddress + uniformOffset atIndex:kBufferFrame];
        if (hasScene) {
            [encoder setRenderPipelineState:surfacePipeline];
            [encoder setDepthStencilState:depthState];
            [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
            [surfaceTable setAddress:definitions.gpuAddress atIndex:kBufferDefinitions];
            [surfaceTable setAddress:occurrences.gpuAddress atIndex:kBufferOccurrences];
            [surfaceTable setAddress:batches.gpuAddress atIndex:kBufferBatches];
            [surfaceTable setAddress:meshletRefs.gpuAddress atIndex:kBufferMeshletRefs];
            [surfaceTable setAddress:occurrenceStates.gpuAddress atIndex:kBufferOccurrenceState];
            [surfaceTable setAddress:faceStates.gpuAddress atIndex:kBufferFaceState];

            // Closed surfaces first, with back faces culled. Open ones show both sides.
            [encoder setCullMode:MTLCullModeBack];
            drawBatches(encoder, 0, openBatchStart);
            [encoder setCullMode:MTLCullModeNone];
            drawBatches(encoder, openBatchStart, batchCount);

            // Ghosts last, blended over everything opaque and writing no depth.
            if (hasGhosts) {
                [encoder setRenderPipelineState:view.uniforms.ghostPickable != 0 ? ghostPickPipeline
                                                                                 : ghostPipeline];
                [encoder setDepthStencilState:ghostDepthState];
                [encoder setCullMode:MTLCullModeBack];
                drawBatches(encoder, 0, openBatchStart);
                [encoder setCullMode:MTLCullModeNone];
                drawBatches(encoder, openBatchStart, batchCount);
            }
        }

        [encoder setRenderPipelineState:pickPipeline];
        [encoder setArgumentTable:tileTable atStages:MTLRenderStageTile];
        [encoder dispatchThreadsPerTile:MTLSizeMake(kTileSize, kTileSize, 1)];
    }

    void beginFrame() {
        // Reusing an allocator requires the frame that last used it to have finished.
        const u64 frame = ++frameCount;
        if (frame > kFramesInFlight) {
            const bool finished = [frameFinished waitUntilSignaledValue:frame - kFramesInFlight
                                                              timeoutMS:kFrameTimeoutMs];
            GEO_ASSERT(finished);
        }
        releaseFinished();
        id<MTL4CommandAllocator> allocator = allocators[frame % kFramesInFlight];
        [allocator reset];
        [commandBuffer beginCommandBufferWithAllocator:allocator];
    }

    void commitAndSignal() {
        id<MTL4CommandBuffer> buffers[] = {commandBuffer};
        if (!traceEnabled()) {
            [queue commit:buffers count:1];
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
        [queue commit:buffers count:1 options:options];
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
    s.device = device;

    NSError* error = nil;
    NSURL* url = [NSURL fileURLWithPath:@(libraryPath)];
    s.library = [device newLibraryWithURL:url error:&error];
    if (!s.library) {
        reportFailure("Could not load the shader library", error);
        return nullptr;
    }

    s.queue = [device newMTL4CommandQueue];
    s.commandBuffer = [device newCommandBuffer];
    s.frameFinished = [device newSharedEvent];
    for (usize i = 0; i < kFramesInFlight; ++i) {
        s.allocators[i] = [device newCommandAllocator];
    }
    if (!s.createPipelines() || !s.createTables()) {
        return nullptr;
    }
    s.frameUniforms = s.makeShared(sizeof(FrameUniforms) * kFramesInFlight);
    [s.residency addAllocation:s.frameUniforms];
    s.occurrenceStates = s.makeShared(sizeof(OccurrenceState));
    s.faceStates = s.makeShared(16);
    [s.residency addAllocation:s.occurrenceStates];
    [s.residency addAllocation:s.faceStates];
    return renderer;
}

void Renderer::attachLayer(CAMetalLayer* layer) {
    [state_->queue addResidencySet:layer.residencySet];
}

void Renderer::syncScene(const scene::Scene& scene) {
    State& s = *state_;

    // Geometry stays in the mapped file: each buffer wraps its segment without copying. See ADR
    // 0002. The deallocator holds the mapping for as long as Metal holds the buffer.
    std::unordered_map<const scene::MappedSegment*, State::SegmentResidency> segments;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        for (const std::shared_ptr<scene::MappedSegment>& segment : scene.document(d).segments()) {
            if (const auto it = s.segmentBuffers.find(segment.get());
                it != s.segmentBuffers.end()) {
                segments.emplace(it->first, it->second);
                continue;
            }
            std::shared_ptr<scene::MappedSegment> owner = segment;
            id<MTLBuffer> buffer =
                [s.device newBufferWithBytesNoCopy:segment->data()
                                            length:segment->size()
                                           options:MTLResourceStorageModeShared |
                                                   MTLResourceHazardTrackingModeUntracked
                                       deallocator:^(void*, NSUInteger) {
                                           (void)owner;
                                       }];
            GEO_ASSERT(buffer != nil);
            NSError* error = nil;
            id<MTLResidencySet> set =
                [s.device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new]
                                                  error:&error];
            GEO_ASSERT(set != nil);
            [set addAllocation:buffer];
            [set commit];
            [s.queue addResidencySet:set];
            // Wiring takes tens of milliseconds for a large segment, and must not stall the engine
            // thread. A frame arriving first simply waits for the pages it needs.
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                [set requestResidency];
            });
            segments.emplace(segment.get(), State::SegmentResidency{buffer, set});
        }
    }

    usize definitionCount = 0, occurrenceCount = 0;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        if (scene.document(d).hasModel()) {
            definitionCount += scene.document(d).model().definitionInfos.size();
            occurrenceCount += scene.document(d).model().occurrences.size();
        }
    }

    id<MTLBuffer> definitions = s.makeShared(definitionCount * sizeof(GpuDefinition));
    id<MTLBuffer> occurrences = s.makeShared(occurrenceCount * sizeof(GpuOccurrence));
    auto* gpuDefinitions = static_cast<GpuDefinition*>(definitions.contents);
    auto* gpuOccurrences = static_cast<GpuOccurrence*>(occurrences.contents);
    // Closed and open surfaces draw with different culling, so each has its own batches.
    struct BatchList {
        std::vector<Batch> batches;
        std::vector<MeshletRef> refs; // packed meshlets not yet in a batch
    };
    BatchList closed, open;
    std::vector<MeshletRef> allRefs;
    const auto flushPacked = [&](BatchList& list, bool all) {
        while (list.refs.size() >= kBatchMeshlets || (all && !list.refs.empty())) {
            const u32 count = u32(std::min<usize>(kBatchMeshlets, list.refs.size()));
            list.batches.push_back({kBatchPacked, 0, u32(allRefs.size()), count});
            allRefs.insert(allRefs.end(), list.refs.begin(), list.refs.begin() + count);
            list.refs.erase(list.refs.begin(), list.refs.begin() + count);
        }
    };

    u32 definitionBase = 0, occurrenceBase = 0, idBase = 0;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        const scene::Document& document = scene.document(d);
        if (!document.hasModel()) {
            continue;
        }
        const format::SegmentView& model = document.model();
        for (u32 i = 0; i < model.definitionInfos.size(); ++i) {
            const format::DefinitionRecord* record = document.geometry(i);
            GpuDefinition& out = gpuDefinitions[definitionBase + i];
            if (record == nullptr) {
                out = {};
                continue;
            }
            const scene::MappedSegment* segment = document.geometrySegment(i);
            id<MTLBuffer> buffer = segments.at(segment).buffer;
            const format::SegmentView& view = segment->view();
            const auto* base = static_cast<const u8*>(segment->data());
            const auto addressOf = [&](const void* pointer) -> u64 {
                return buffer.gpuAddress + u64(static_cast<const u8*>(pointer) - base);
            };
            out.meshlets = addressOf(view.meshlets.data() + record->firstMeshlet);
            out.vertices = addressOf(view.vertices.data() + record->firstVertex);
            out.highBits = (record->flags & kDefinitionWide) != 0
                               ? addressOf(view.highBits.data() + record->firstHighBits)
                               : 0;
            out.triangles = addressOf(view.triangles.data() + record->firstTriangle);
            out.boundsMin =
                simd_make_float3(record->boundsMin[0], record->boundsMin[1], record->boundsMin[2]);
            out.decodeScale = simd_make_float3(record->decodeScale[0], record->decodeScale[1],
                                               record->decodeScale[2]);
            out.boundsScale = simd_make_float3(record->boundsScale[0], record->boundsScale[1],
                                               record->boundsScale[2]);
            out.boundsStep = simd_reduce_max(out.boundsScale);
            out.flags = record->flags;
        }

        for (u32 i = 0; i < model.occurrences.size(); ++i) {
            const format::OccurrenceRecord& record = model.occurrences[i];
            const std::array<float, 12>& t = record.transform;
            const u32 global = occurrenceBase + i;
            GpuOccurrence& out = gpuOccurrences[global];
            out.rows[0] = simd_make_float4(t[0], t[3], t[6], t[9]);
            out.rows[1] = simd_make_float4(t[1], t[4], t[7], t[10]);
            out.rows[2] = simd_make_float4(t[2], t[5], t[8], t[11]);
            out.definition = definitionBase + record.definition;
            out.idBase = idBase + document.idBase(i);

            const format::DefinitionRecord* geometry = document.geometry(record.definition);
            if (geometry == nullptr) {
                continue;
            }
            BatchList& target = (geometry->flags & kDefinitionOpen) != 0 ? open : closed;
            if (geometry->meshletCount < kPackedMeshletLimit) {
                for (u32 m = 0; m < geometry->meshletCount; ++m) {
                    target.refs.push_back({global, m});
                }
                flushPacked(target, false);
                continue;
            }
            for (u32 first = 0; first < geometry->meshletCount; first += kBatchMeshlets) {
                const u32 count = std::min<u32>(kBatchMeshlets, geometry->meshletCount - first);
                target.batches.push_back({kBatchRun, global, first, count});
            }
        }
        definitionBase += u32(model.definitionInfos.size());
        occurrenceBase += u32(model.occurrences.size());
        idBase += document.idCount();
    }

    flushPacked(closed, true);
    flushPacked(open, true);
    const std::vector<Batch>& closedBatches = closed.batches;
    const std::vector<Batch>& openBatches = open.batches;
    const usize batchCount = closedBatches.size() + openBatches.size();
    id<MTLBuffer> batches = s.makeShared(batchCount * sizeof(Batch));
    auto* gpuBatches = static_cast<Batch*>(batches.contents);
    std::copy(closedBatches.begin(), closedBatches.end(), gpuBatches);
    std::copy(openBatches.begin(), openBatches.end(), gpuBatches + closedBatches.size());
    id<MTLBuffer> meshletRefs = s.makeShared(allRefs.size() * sizeof(MeshletRef));
    std::copy(allRefs.begin(), allRefs.end(), static_cast<MeshletRef*>(meshletRefs.contents));

    const usize drawCount = batchCount / kMaxBatchesPerDraw + 2;
    id<MTLBuffer> drawBases = s.makeShared(drawCount * sizeof(u32));
    auto* bases = static_cast<u32*>(drawBases.contents);
    for (usize i = 0; i < drawCount; ++i) {
        bases[i] = u32(i * kMaxBatchesPerDraw);
    }

    // The last frame may still read the old tables, so they go when it finishes.
    State::Retired retired;
    retired.frame = s.frameCount;
    for (id<MTLBuffer> old :
         {s.definitions, s.occurrences, s.batches, s.drawBases, s.meshletRefs}) {
        if (old != nil) {
            retired.buffers.push_back(old);
        }
    }
    for (const auto& [segment, residency] : s.segmentBuffers) {
        if (!segments.contains(segment)) {
            retired.sets.push_back(residency.set);
        }
    }
    if (!retired.buffers.empty() || !retired.sets.empty()) {
        s.retired.push_back(std::move(retired));
    }

    s.segmentBuffers = std::move(segments);
    s.definitions = definitions;
    s.occurrences = occurrences;
    s.batches = batches;
    s.meshletRefs = meshletRefs;
    s.drawBases = drawBases;
    s.batchCount = batchCount;
    s.openBatchStart = closedBatches.size();
    s.hasScene = batchCount != 0;
    for (id<MTLBuffer> resource : {definitions, occurrences, batches, drawBases, meshletRefs}) {
        [s.residency addAllocation:resource];
    }
    [s.residency commit];
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
    State::Retired retired;
    retired.frame = s.frameCount;
    const auto ensure = [&](id<MTLBuffer> __strong& buffer, usize bytes) {
        if (buffer.length >= bytes) {
            return;
        }
        retired.buffers.push_back(buffer);
        buffer = s.makeShared(std::max<usize>(bytes, buffer.length * 2));
        [s.residency addAllocation:buffer];
    };
    ensure(s.occurrenceStates, words.size_bytes());
    ensure(s.faceStates, faces.size_bytes());
    if (!retired.buffers.empty()) {
        s.retired.push_back(std::move(retired));
        [s.residency commit];
    }
    std::memcpy(s.occurrenceStates.contents, words.data(), words.size_bytes());
    std::memcpy(s.faceStates.contents, faces.data(), faces.size_bytes());

    s.hasGhosts = false;
    for (const OccurrenceState& word : words) {
        if ((word.flags & (kStateGhost | kStateFaceGhost)) != 0) {
            s.hasGhosts = true;
            break;
        }
    }
}

void Renderer::resize(u32 width, u32 height) {
    State& s = *state_;
    if (width == s.width && height == s.height) {
        return;
    }
    if (s.pickIdBuffer) {
        [s.residency removeAllocation:s.pickIdBuffer];
        [s.residency removeAllocation:s.pickDepthBuffer];
        [s.residency removeAllocation:s.offscreenColor];
    }
    s.width = width;
    s.height = height;
    s.pass = nil;

    const MTLTextureUsage target = MTLTextureUsageRenderTarget;
    s.colorMsaa = s.makeTarget(kColorFormat, kSampleCount, MTLStorageModeMemoryless, target);
    s.idMsaa = s.makeTarget(kIdFormat, kSampleCount, MTLStorageModeMemoryless, target);
    s.depthCopyMsaa =
        s.makeTarget(kDepthCopyFormat, kSampleCount, MTLStorageModeMemoryless, target);
    s.depthMsaa = s.makeTarget(kDepthFormat, kSampleCount, MTLStorageModeMemoryless, target);
    s.offscreenColor = s.makeTarget(kColorFormat, 1, MTLStorageModeShared, target);

    const auto makeLinear = [&](MTLPixelFormat format, id<MTLBuffer> __strong& buffer,
                                id<MTLTexture> __strong& texture, NSUInteger& rowBytes) {
        const NSUInteger alignment = [s.device minimumLinearTextureAlignmentForPixelFormat:format];
        rowBytes = (NSUInteger(width) * 4 + alignment - 1) & ~(alignment - 1);
        buffer = s.makeShared(rowBytes * height);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderWrite;
        d.resourceOptions = buffer.resourceOptions;
        texture = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:rowBytes];
    };
    makeLinear(kIdFormat, s.pickIdBuffer, s.pickIds, s.pickIdRowBytes);
    makeLinear(kDepthCopyFormat, s.pickDepthBuffer, s.pickDepths, s.pickDepthRowBytes);

    [s.tileTable setTexture:s.pickIds.gpuResourceID atIndex:kTexturePickIds];
    [s.tileTable setTexture:s.pickDepths.gpuResourceID atIndex:kTexturePickDepths];
    [s.residency addAllocation:s.pickIdBuffer];
    [s.residency addAllocation:s.pickDepthBuffer];
    [s.residency addAllocation:s.offscreenColor];
    [s.residency commit];
    [s.residency requestResidency];
}

void Renderer::render(id<CAMetalDrawable> drawable, const render::ViewState& view) {
    State& s = *state_;
    s.beginFrame();
    id<MTL4RenderCommandEncoder> encoder =
        [s.commandBuffer renderCommandEncoderWithDescriptor:s.passDescriptor(drawable.texture)];
    s.encodeScene(encoder, view, s.frameCount);
    [encoder endEncoding];
    [s.commandBuffer endCommandBuffer];

    [s.queue waitForDrawable:drawable];
    s.commitAndSignal();
    [s.queue signalDrawable:drawable];
    [s.queue signalEvent:s.frameFinished value:s.frameCount];
    [drawable present];
}

std::vector<u8> Renderer::renderOffscreen(const render::ViewState& view) {
    State& s = *state_;
    s.beginFrame();
    id<MTL4RenderCommandEncoder> encoder =
        [s.commandBuffer renderCommandEncoderWithDescriptor:s.passDescriptor(s.offscreenColor)];
    s.encodeScene(encoder, view, s.frameCount);
    [encoder endEncoding];
    [s.commandBuffer endCommandBuffer];
    s.commitAndSignal();
    [s.queue signalEvent:s.frameFinished value:s.frameCount];
    const bool finished = [s.frameFinished waitUntilSignaledValue:s.frameCount
                                                        timeoutMS:kFrameTimeoutMs * 10];
    GEO_ASSERT(finished);

    std::vector<u8> pixels(usize(s.width) * s.height * 4);
    [s.offscreenColor getBytes:pixels.data()
                   bytesPerRow:NSUInteger(s.width) * 4
                    fromRegion:MTLRegionMake2D(0, 0, s.width, s.height)
                   mipmapLevel:0];
    return pixels;
}

std::optional<PixelHit> Renderer::readPixel(int x, int y, const render::DepthDecoder& depth) const {
    const State& s = *state_;
    if (x < 0 || y < 0 || u32(x) >= s.width || u32(y) >= s.height || !s.pickIdBuffer) {
        return std::nullopt;
    }
    const auto* ids = static_cast<const u8*>(s.pickIdBuffer.contents);
    const auto* depths = static_cast<const u8*>(s.pickDepthBuffer.contents);
    u32 id = 0;
    float value = 0.0f;
    std::memcpy(&id, ids + usize(y) * s.pickIdRowBytes + usize(x) * 4, sizeof(id));
    std::memcpy(&value, depths + usize(y) * s.pickDepthRowBytes + usize(x) * 4, sizeof(value));
    if (id == 0) {
        return std::nullopt;
    }
    return PixelHit{id, depth.viewDepth(value)};
}

} // namespace geo::gpu
