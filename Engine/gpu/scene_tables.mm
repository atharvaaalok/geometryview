#include "gpu/scene_tables.h"

#include "base/assert.h"
#include "shaders/shared/gpu_types.h"

#include <algorithm>

namespace geo::gpu {
namespace {

// Definitions with fewer meshlets than this share packed batches with others.
constexpr u32 kPackedMeshletLimit = kBatchMeshlets / 2;

// Batches for one kind of draw. Small definitions' meshlets wait in `refs` until a whole packed
// batch of them is ready.
struct BatchList {
    std::vector<Batch> batches;
    std::vector<MeshletRef> refs;

    void flushPacked(std::vector<MeshletRef>& allRefs, bool all) {
        while (refs.size() >= kBatchMeshlets || (all && !refs.empty())) {
            const u32 count = u32(std::min<usize>(kBatchMeshlets, refs.size()));
            batches.push_back({kBatchPacked, 0, u32(allRefs.size()), count});
            allRefs.insert(allRefs.end(), refs.begin(), refs.begin() + count);
            refs.erase(refs.begin(), refs.begin() + count);
        }
    }

    void add(u32 occurrence, u32 meshletCount, std::vector<MeshletRef>& allRefs) {
        if (meshletCount < kPackedMeshletLimit) {
            for (u32 m = 0; m < meshletCount; ++m) {
                refs.push_back({occurrence, m});
            }
            flushPacked(allRefs, false);
            return;
        }
        for (u32 first = 0; first < meshletCount; first += kBatchMeshlets) {
            batches.push_back({kBatchRun, occurrence, first,
                               std::min<u32>(kBatchMeshlets, meshletCount - first)});
        }
    }
};

} // namespace

void SceneTables::sync(GpuContext& context, Retirement& retirement, const scene::Scene& scene) {
    // Geometry stays in the mapped file: each buffer wraps its segment without copying. See ADR
    // 0002. The deallocator holds the mapping for as long as Metal holds the buffer.
    std::unordered_map<const scene::Segment*, SegmentResidency> segments;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        for (const std::shared_ptr<scene::Segment>& segment : scene.document(d).segments()) {
            if (const auto it = segments_.find(segment.get()); it != segments_.end()) {
                segments.emplace(it->first, it->second);
                continue;
            }
            std::shared_ptr<scene::Segment> owner = segment;
            id<MTLBuffer> buffer =
                [context.device newBufferWithBytesNoCopy:segment->data()
                                                  length:segment->size()
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceHazardTrackingModeUntracked
                                             deallocator:^(void*, NSUInteger) {
                                                 (void)owner;
                                             }];
            GEO_ASSERT(buffer != nil);
            NSError* error = nil;
            id<MTLResidencySet> set =
                [context.device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new]
                                                        error:&error];
            GEO_ASSERT(set != nil);
            [set addAllocation:buffer];
            [set commit];
            [context.queue addResidencySet:set];
            // Wiring takes tens of milliseconds for a large segment, and must not stall the engine
            // thread. A frame arriving first simply waits for the pages it needs.
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                [set requestResidency];
            });
            segments.emplace(segment.get(), SegmentResidency{buffer, set});
        }
    }

    usize definitionCount = 0, occurrenceCount = 0;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        if (scene.document(d).hasModel()) {
            definitionCount += scene.document(d).model().definitionInfos.size();
            occurrenceCount += scene.document(d).model().occurrences.size();
        }
    }

    id<MTLBuffer> definitions = context.makeShared(definitionCount * sizeof(GpuDefinition));
    id<MTLBuffer> occurrences = context.makeShared(occurrenceCount * sizeof(GpuOccurrence));
    auto* gpuDefinitions = static_cast<GpuDefinition*>(definitions.contents);
    auto* gpuOccurrences = static_cast<GpuOccurrence*>(occurrences.contents);
    BatchList closed, open, edges;
    std::vector<MeshletRef> allRefs, edgeRefs;

    u32 definitionBase = 0, occurrenceBase = 0, idBase = 0;
    u64 triangles = 0, edgeSegments = 0;
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
            const scene::Segment* segment = document.geometrySegment(i);
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
            out.edgeSegments = addressOf(view.edgeSegments.data() + record->firstEdgeSegment);
            out.edgeChunks = addressOf(view.edgeChunks.data() + record->firstEdgeChunk);
            out.vertexCount = record->vertexCount;
            out.faceCount = record->faceCount;
            out.boundsMin =
                simd_make_float3(record->boundsMin[0], record->boundsMin[1], record->boundsMin[2]);
            out.decodeScale = simd_make_float3(record->decodeScale[0], record->decodeScale[1],
                                               record->decodeScale[2]);
            out.boundsScale = simd_make_float3(record->boundsScale[0], record->boundsScale[1],
                                               record->boundsScale[2]);
            out.boundsStep = simd_reduce_max(out.boundsScale);
            out.flags = record->flags;
            out.edgeLift = document.edgeLift(*record);
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
            triangles += geometry->triangleCount;
            edgeSegments += geometry->edgeSegmentCount;
            BatchList& target = (geometry->flags & kDefinitionOpen) != 0 ? open : closed;
            target.add(global, geometry->meshletCount, allRefs);
            edges.add(global, geometry->edgeChunkCount, edgeRefs);
        }
        definitionBase += u32(model.definitionInfos.size());
        occurrenceBase += u32(model.occurrences.size());
        idBase += document.idCount();
    }

    closed.flushPacked(allRefs, true);
    open.flushPacked(allRefs, true);
    edges.flushPacked(edgeRefs, true);
    const usize batchCount = closed.batches.size() + open.batches.size();
    id<MTLBuffer> batches = context.makeShared(batchCount * sizeof(Batch));
    auto* gpuBatches = static_cast<Batch*>(batches.contents);
    std::copy(closed.batches.begin(), closed.batches.end(), gpuBatches);
    std::copy(open.batches.begin(), open.batches.end(), gpuBatches + closed.batches.size());
    id<MTLBuffer> meshletRefs = context.makeShared(allRefs.size() * sizeof(MeshletRef));
    std::copy(allRefs.begin(), allRefs.end(), static_cast<MeshletRef*>(meshletRefs.contents));

    id<MTLBuffer> edgeBatches = context.makeShared(edges.batches.size() * sizeof(Batch));
    std::copy(edges.batches.begin(), edges.batches.end(),
              static_cast<Batch*>(edgeBatches.contents));
    id<MTLBuffer> edgeChunkRefs = context.makeShared(edgeRefs.size() * sizeof(MeshletRef));
    std::copy(edgeRefs.begin(), edgeRefs.end(), static_cast<MeshletRef*>(edgeChunkRefs.contents));

    const usize drawCount = std::max(batchCount, edges.batches.size()) / kMaxBatchesPerDraw + 2;
    id<MTLBuffer> drawBases = context.makeShared(drawCount * sizeof(u32));
    auto* bases = static_cast<u32*>(drawBases.contents);
    for (usize i = 0; i < drawCount; ++i) {
        bases[i] = u32(i * kMaxBatchesPerDraw);
    }

    // The last frame may still read the old tables, so they go when it finishes.
    std::vector<id<MTLBuffer>> oldBuffers;
    for (id<MTLBuffer> old : {definitions_, occurrences_, batches_, drawBases_, meshletRefs_,
                              edgeBatches_, edgeRefs_}) {
        if (old != nil) {
            oldBuffers.push_back(old);
        }
    }
    std::vector<id<MTLResidencySet>> oldSets;
    for (const auto& [segment, residency] : segments_) {
        if (!segments.contains(segment)) {
            oldSets.push_back(residency.set);
        }
    }
    retirement.retire(context.frameCount, std::move(oldBuffers), std::move(oldSets));

    segments_ = std::move(segments);
    definitions_ = definitions;
    occurrences_ = occurrences;
    batches_ = batches;
    meshletRefs_ = meshletRefs;
    drawBases_ = drawBases;
    edgeBatches_ = edgeBatches;
    edgeRefs_ = edgeChunkRefs;
    surfaceBatchCount_ = batchCount;
    openBatchStart_ = closed.batches.size();
    edgeBatchCount_ = edges.batches.size();
    triangles_ = triangles;
    edgeSegments_ = edgeSegments;
    for (id<MTLBuffer> resource :
         {definitions, occurrences, batches, drawBases, meshletRefs, edgeBatches, edgeChunkRefs}) {
        [context.residency addAllocation:resource];
    }
    [context.residency commit];
}

} // namespace geo::gpu
