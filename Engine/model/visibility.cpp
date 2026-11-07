#include "model/visibility.h"

#include <cmath>

namespace geo::model {

std::optional<Aabb> occurrenceBox(const scene::Document& document,
                                  const format::OccurrenceRecord& occurrence) {
    const format::DefinitionInfo& info = document.model().definitionInfos[occurrence.definition];
    const format::DefinitionRecord* geometry = document.geometry(occurrence.definition);
    const auto& lo = geometry != nullptr ? geometry->boundsMin : info.boundsMin;
    const auto& hi = geometry != nullptr ? geometry->boundsMax : info.boundsMax;
    if (lo[0] > hi[0] || !std::isfinite(lo[0]) || !std::isfinite(hi[0])) {
        return std::nullopt;
    }
    // A box turns into the box around its centre, moved, with each half extent the absolute
    // matrix applied to the old one: exact for the box's corners, at a fraction of eight
    // transforms.
    const std::array<float, 12>& t = occurrence.transform;
    const double c[3] = {0.5 * (double(lo[0]) + hi[0]), 0.5 * (double(lo[1]) + hi[1]),
                         0.5 * (double(lo[2]) + hi[2])};
    const double e[3] = {0.5 * (double(hi[0]) - lo[0]), 0.5 * (double(hi[1]) - lo[1]),
                         0.5 * (double(hi[2]) - lo[2])};
    Vec3 center{};
    Vec3 extent{};
    for (int row = 0; row < 3; ++row) {
        center[row] = t[row] * c[0] + t[3 + row] * c[1] + t[6 + row] * c[2] + t[9 + row];
        extent[row] =
            std::abs(t[row]) * e[0] + std::abs(t[3 + row]) * e[1] + std::abs(t[6 + row]) * e[2];
    }
    return Aabb{center - extent, center + extent};
}

std::optional<Aabb> visibleBounds(const scene::Scene& scene, const FaceState& state) {
    const std::span<const OccurrenceState> words = state.occurrenceWords();
    return boundsWhere(scene, [&](u32 occurrence) {
        return occurrence >= words.size() || (words[occurrence].flags & kStateHidden) == 0;
    });
}

std::vector<NodeVisibility> nodeStates(const scene::Scene& scene, const FaceState& state,
                                       u32 document) {
    if (document >= scene.documentCount() || !scene.document(document).hasModel()) {
        return {};
    }
    const format::SegmentView& model = scene.document(document).model();
    const u32 base = scene.occurrenceBase(document);
    const std::span<const OccurrenceState> words = state.occurrenceWords();

    // Running counts over the document's occurrences, so each node, which covers a contiguous
    // range of them, is decided by three subtractions.
    const usize count = model.occurrences.size();
    std::vector<u32> hidden(count + 1, 0), ghosted(count + 1, 0), touched(count + 1, 0);
    for (usize o = 0; o < count; ++o) {
        const u32 global = base + u32(o);
        const u32 flags = global < words.size() ? words[global].flags : 0;
        const bool any = global < words.size() && state.anyFace(global, kStateHidden | kStateGhost);
        hidden[o + 1] = hidden[o] + ((flags & kStateHidden) != 0 ? 1 : 0);
        ghosted[o + 1] = ghosted[o] + ((flags & kStateGhost) != 0 ? 1 : 0);
        touched[o + 1] = touched[o] + (any ? 1 : 0);
    }

    std::vector<NodeVisibility> states(model.nodes.size(), NodeVisibility::Shown);
    for (usize n = 0; n < model.nodes.size(); ++n) {
        const format::NodeRecord& node = model.nodes[n];
        const u32 first = std::min<u32>(node.firstOccurrence, u32(count));
        const u32 end = std::min<u32>(node.occurrenceEnd, u32(count));
        if (end <= first) {
            continue;
        }
        const u32 total = end - first;
        if (hidden[end] - hidden[first] == total) {
            states[n] = NodeVisibility::Hidden;
        } else if (ghosted[end] - ghosted[first] == total) {
            states[n] = NodeVisibility::Ghosted;
        } else if (touched[end] - touched[first] != 0) {
            states[n] = NodeVisibility::Mixed;
        }
    }
    return states;
}

} // namespace geo::model
