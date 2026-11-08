// Fuzzes the streamed cache reader, as the browser runs it: a cache file, untrusted, arriving in
// pieces of any size, its segments placed by a sink as on the GPU or kept in memory where the sink
// declines. Any bytes must be refused or give segments that are safe to walk, and nothing may be
// written outside a placement.
#include "scene/segment.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace geo;

namespace {

struct Placed final : scene::Placement {
    std::vector<u8> bytes;
};

// Places segments up to 1 MB, as a GPU with little room would; larger ones it declines.
struct Sink final : scene::SegmentSink {
    std::shared_ptr<scene::Placement> place(u64 size) override {
        if (size > (1u << 20)) {
            return nullptr;
        }
        auto placed = std::make_shared<Placed>();
        placed->bytes.resize(usize(size));
        return placed;
    }
    void write(scene::Placement& placement, u64 offset, std::span<const u8> bytes) override {
        std::vector<u8>& to = static_cast<Placed&>(placement).bytes;
        if (offset > to.size() || bytes.size() > to.size() - offset) {
            __builtin_trap();
        }
        std::memcpy(to.data() + offset, bytes.data(), bytes.size());
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) {
        return 0;
    }
    // The first byte picks the pieces' size and whether the host knows the file's size.
    const usize piece = 1 + usize(data[0] & 0x7F) * 97;
    const bool sized = (data[0] & 0x80) != 0;
    const std::span<const u8> file(data + 1, size - 1);
    Sink sink;
    u64 sum = 0;
    scene::CacheStream reader(&sink, [&](std::shared_ptr<scene::Segment> segment) {
        const format::SegmentView& view = segment->view();
        for (const format::DefinitionRecord& d : view.definitions) {
            for (u32 m = 0; m < d.meshletCount; ++m) {
                sum += view.meshlets[d.firstMeshlet + m].vertexOffset;
            }
            for (u32 c = 0; c < d.edgeChunkCount; ++c) {
                sum += view.edgeChunks[d.firstEdgeChunk + c].segmentCount;
            }
        }
        for (const format::NodeRecord& node : view.nodes) {
            sum += node.nameLength == 0 ? 0 : u8(view.strings[node.nameOffset]);
        }
    });
    if (sized) {
        reader.expectSize(file.size());
    }
    std::string error;
    for (usize at = 0; at < file.size(); at += piece) {
        if (!reader.append(file.subspan(at, std::min(piece, file.size() - at)), error)) {
            return 0;
        }
    }
    (void)reader.finish(error);
    return sum == 0xFFFFFFFFFFFFFFFFull ? 1 : 0;
}
