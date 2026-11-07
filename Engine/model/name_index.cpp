#include "model/name_index.h"

#include "base/parallel.h"

#include <algorithm>
#include <cstring>

namespace geo::model {
namespace {

// Below this many bytes a single scan beats waking other cores.
constexpr usize kParallelBytes = 1 << 20;

char lower(char c) {
    return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
}

} // namespace

NameIndex::NameIndex(const format::SegmentView& model) {
    folded_.reserve(model.strings.size() + model.nodes.size());
    starts_.reserve(model.nodes.size() + 1);
    for (const format::NodeRecord& node : model.nodes) {
        starts_.push_back(u32(folded_.size()));
        for (u32 i = 0; i < node.nameLength; ++i) {
            folded_.push_back(lower(model.strings[node.nameOffset + i]));
        }
        folded_.push_back('\0');
    }
    starts_.push_back(u32(folded_.size()));
}

void NameIndex::scan(std::string_view needle, u32 first, u32 last, std::vector<u32>& out) const {
    const char* base = folded_.data();
    const char* at = base + starts_[first];
    const char* end = base + starts_[last];
    u32 node = first;
    while (at < end) {
        const void* found = memmem(at, usize(end - at), needle.data(), needle.size());
        if (found == nullptr) {
            return;
        }
        // Matches arrive in order, so the owning node is found by walking forward.
        const u32 offset = u32(static_cast<const char*>(found) - base);
        while (starts_[node + 1] <= offset) {
            ++node;
        }
        out.push_back(node);
        // The rest of this name is skipped, so a node matches once.
        at = base + starts_[++node];
    }
}

u32 NameIndex::search(std::string_view query, std::span<u32> results) const {
    const u32 nodes = u32(starts_.size()) - 1;
    if (query.empty() || nodes == 0) {
        return 0;
    }
    std::string needle(query);
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);

    const usize ranges = folded_.size() < kParallelBytes ? 1 : parallelRangeCount(nodes, 1024);
    std::vector<std::vector<u32>> found(ranges);
    parallelFor(ranges, [&](usize range) {
        scan(needle, u32(nodes * range / ranges), u32(nodes * (range + 1) / ranges), found[range]);
    });

    u32 matched = 0;
    for (const std::vector<u32>& part : found) {
        for (const u32 node : part) {
            if (matched < results.size()) {
                results[matched] = node;
            }
            ++matched;
        }
    }
    return matched;
}

} // namespace geo::model
