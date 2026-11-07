// Fuzzes the cache reader, which treats a cache file as untrusted: any bytes must validate or be
// rejected, and what validates must be safe to walk.
#include "format/blob.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace geo;
using namespace geo::format;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Copied so sections are as aligned as a mapped file's would be.
    std::vector<u64> aligned((size + 7) / 8);
    std::memcpy(aligned.data(), data, size);
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(aligned.data()), size);
    std::string error;
    SegmentView view;
    if (!parseSegment(bytes, view, error)) {
        return 0;
    }
    u64 sum = 0;
    for (const DefinitionRecord& d : view.definitions) {
        for (u32 m = 0; m < d.meshletCount; ++m) {
            const Meshlet& meshlet = view.meshlets[d.firstMeshlet + m];
            sum += view.vertices[d.firstVertex + meshlet.vertexOffset].x;
        }
        for (u32 c = 0; c < d.edgeChunkCount; ++c) {
            const EdgeChunk& chunk = view.edgeChunks[d.firstEdgeChunk + c];
            if (chunk.segmentCount > 0) {
                sum += view.edgeSegments[d.firstEdgeSegment + chunk.firstSegment].a;
            }
        }
    }
    for (const NodeRecord& node : view.nodes) {
        sum += u64(
            view.strings.data()[node.nameOffset == view.strings.size() ? 0 : node.nameOffset] != 0);
    }
    return sum == 0xFFFFFFFFFFFFFFFFull ? 1 : 0;
}
