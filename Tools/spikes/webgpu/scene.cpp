#include "scene.h"

#include "format/blob.h"

#include <algorithm>
#include <cmath>

namespace geo::spike {
namespace {

template <typename T>
const u8* bytesOf(std::span<const T> items, usize first) {
    return reinterpret_cast<const u8*>(items.data() + first);
}

} // namespace

bool buildScene(std::span<const u8> file, u32 copies, SceneData& scene, std::string& error) {
    std::span<const format::SegmentEntry> entries;
    if (!format::parseHeader(file, entries, error)) {
        return false;
    }
    std::vector<format::SegmentView> views(entries.size());
    const format::SegmentView* model = nullptr;
    for (usize i = 0; i < entries.size(); ++i) {
        if (!format::parseSegment(file.subspan(usize(entries[i].offset), usize(entries[i].size)),
                                  views[i], error)) {
            return false;
        }
        if (views[i].hasModel()) {
            model = &views[i];
        }
    }
    if (model == nullptr) {
        error = "The file has no model.";
        return false;
    }

    // The finest geometry of each definition, and the segment it is in.
    struct Chosen {
        const format::DefinitionRecord* record = nullptr;
        const format::SegmentView* view = nullptr;
    };
    std::vector<Chosen> chosen(model->definitionInfos.size());
    for (const format::SegmentView& view : views) {
        for (const format::DefinitionRecord& record : view.definitions) {
            if (record.definition < chosen.size() &&
                (chosen[record.definition].record == nullptr ||
                 record.level > chosen[record.definition].record->level)) {
                chosen[record.definition] = {&record, &view};
            }
        }
    }

    // Definitions, with each one's geometry appended to the four streams.
    std::array<u64, kStreams> elements{};
    scene.definitions.resize(chosen.size());
    for (usize d = 0; d < chosen.size(); ++d) {
        GpuDefinition& out = scene.definitions[d];
        out = {};
        const format::DefinitionRecord* r = chosen[d].record;
        if (r == nullptr) {
            continue;
        }
        const format::SegmentView& view = *chosen[d].view;
        out.meshletBase = u32(elements[kStreamMeshlets]);
        out.vertexBase = u32(elements[kStreamVertices]);
        out.highBitsBase = u32(elements[kStreamHighBits]);
        out.triangleBase = u32(elements[kStreamTriangles]);
        out.edgeChunkBase = u32(elements[kStreamEdgeChunks]);
        out.edgeSegmentBase = u32(elements[kStreamEdgeSegments]);
        out.faceCount = r->faceCount;
        std::copy_n(r->boundsMin.data(), 3, out.boundsMin);
        std::copy_n(r->decodeScale.data(), 3, out.decodeScale);
        std::copy_n(r->boundsScale.data(), 3, out.boundsScale);
        out.boundsStep = std::max({r->boundsScale[0], r->boundsScale[1], r->boundsScale[2]});
        out.flags = r->flags;
        out.vertexCount = r->vertexCount;

        const bool wide = (r->flags & kDefinitionWide) != 0;
        auto add = [&](Stream stream, const u8* data, u64 count, u64 stride) {
            scene.pieces.push_back({stream, elements[stream] * stride, data, count * stride});
            elements[stream] += count;
        };
        add(kStreamMeshlets, bytesOf(view.meshlets, r->firstMeshlet), r->meshletCount,
            sizeof(Meshlet));
        add(kStreamVertices, bytesOf(view.vertices, r->firstVertex), r->vertexCount,
            sizeof(PackedVertex));
        if (wide) {
            add(kStreamHighBits, bytesOf(view.highBits, r->firstHighBits), r->vertexCount, 4);
        }
        add(kStreamTriangles, bytesOf(view.triangles, r->firstTriangle), r->triangleCount, 4);
        add(kStreamEdgeChunks, bytesOf(view.edgeChunks, r->firstEdgeChunk), r->edgeChunkCount,
            sizeof(EdgeChunk));
        add(kStreamEdgeSegments, bytesOf(view.edgeSegments, r->firstEdgeSegment),
            r->edgeSegmentCount, sizeof(EdgeSegment));
    }
    scene.streamBytes = {elements[kStreamMeshlets] * sizeof(Meshlet),
                         elements[kStreamVertices] * sizeof(PackedVertex),
                         elements[kStreamHighBits] * 4,
                         elements[kStreamTriangles] * 4,
                         elements[kStreamEdgeChunks] * sizeof(EdgeChunk),
                         elements[kStreamEdgeSegments] * sizeof(EdgeSegment)};

    // The model's bounds, from each occurrence's transformed definition bounds.
    std::array<float, 3> low = {INFINITY, INFINITY, INFINITY};
    std::array<float, 3> high = {-INFINITY, -INFINITY, -INFINITY};
    for (const format::OccurrenceRecord& o : model->occurrences) {
        const format::DefinitionInfo& info = model->definitionInfos[o.definition];
        for (u32 corner = 0; corner < 8; ++corner) {
            const float p[3] = {(corner & 1) ? info.boundsMax[0] : info.boundsMin[0],
                                (corner & 2) ? info.boundsMax[1] : info.boundsMin[1],
                                (corner & 4) ? info.boundsMax[2] : info.boundsMin[2]};
            for (u32 axis = 0; axis < 3; ++axis) {
                const float w = o.transform[axis] * p[0] + o.transform[3 + axis] * p[1] +
                                o.transform[6 + axis] * p[2] + o.transform[9 + axis];
                low[axis] = std::min(low[axis], w);
                high[axis] = std::max(high[axis], w);
            }
        }
    }
    const float spacing = (high[0] - low[0]) * 1.1f;

    // Occurrences, copies side by side, and every meshlet of each as one cull instance.
    copies = std::max(copies, 1u);
    u32 idBase = 0;
    for (u32 copy = 0; copy < copies; ++copy) {
        for (const format::OccurrenceRecord& o : model->occurrences) {
            const auto occurrence = u32(scene.occurrences.size());
            GpuOccurrence out{};
            for (u32 row = 0; row < 3; ++row) {
                out.rows[row][0] = o.transform[row];
                out.rows[row][1] = o.transform[3 + row];
                out.rows[row][2] = o.transform[6 + row];
                out.rows[row][3] = o.transform[9 + row] + (row == 0 ? spacing * float(copy) : 0.f);
            }
            out.definition = o.definition;
            out.idBase = idBase;
            idBase += model->definitionInfos[o.definition].faceCount;
            scene.occurrences.push_back(out);

            const format::DefinitionRecord* r = chosen[o.definition].record;
            if (r == nullptr) {
                continue;
            }
            const bool open = (r->flags & kDefinitionOpen) != 0;
            const std::span<const Meshlet> meshlets =
                chosen[o.definition].view->meshlets.subspan(r->firstMeshlet, r->meshletCount);
            for (u32 m = 0; m < r->meshletCount; ++m) {
                scene.instances.push_back({occurrence, m});
                const u64 clusters = (meshlets[m].triangleCount + 15u) / 16u;
                (open ? scene.openClusters : scene.closedClusters) += clusters;
                scene.closedInstances += open ? 0 : 1;
                (open ? scene.openIndices : scene.closedIndices) += meshlets[m].triangleCount * 3u;
            }
            scene.triangles += r->triangleCount;
            for (u32 c = 0; c < r->edgeChunkCount; ++c) {
                scene.edgeInstances.push_back({occurrence, c});
            }
        }
    }
    scene.boundsMin = low;
    scene.boundsMax = {high[0] + spacing * float(copies - 1), high[1], high[2]};
    return true;
}

} // namespace geo::spike
