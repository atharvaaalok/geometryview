// What is shown, worked out from the face state: the bounds that framing and rotation follow, and
// the state each tree row reports.
#pragma once

#include "base/types.h"
#include "math/geometry.h"
#include "model/face_state.h"
#include "scene/scene.h"

#include <optional>
#include <vector>

namespace geo::model {

/// The world box of an occurrence, from its definition's bounds, or nullopt while they are
/// unknown.
std::optional<Aabb> occurrenceBox(const scene::Document& document,
                                  const format::OccurrenceRecord& occurrence);

/// The union of the world boxes of the occurrences `include` accepts, by global index, or nullopt
/// if it accepts none with known bounds.
template <typename Include>
std::optional<Aabb> boundsWhere(const scene::Scene& scene, Include&& include) {
    Aabb box{simd_make_double3(INFINITY, INFINITY, INFINITY),
             simd_make_double3(-INFINITY, -INFINITY, -INFINITY)};
    bool any = false;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        const scene::Document& document = scene.document(d);
        if (!document.hasModel()) {
            continue;
        }
        const u32 base = scene.occurrenceBase(d);
        const auto& occurrences = document.model().occurrences;
        for (u32 o = 0; o < occurrences.size(); ++o) {
            if (!include(base + o)) {
                continue;
            }
            if (const std::optional<Aabb> part = occurrenceBox(document, occurrences[o])) {
                box.min = simd_min(box.min, part->min);
                box.max = simd_max(box.max, part->max);
                any = true;
            }
        }
    }
    return any ? std::optional<Aabb>(box) : std::nullopt;
}

/// The bounds of everything not wholly hidden: what framing and rotation should treat as the model.
std::optional<Aabb> visibleBounds(const scene::Scene& scene, const FaceState& state);

/// What a tree row reports about what lies under it.
enum class NodeVisibility : u8 {
    Shown = 0,   // nothing hidden or ghosted
    Hidden = 1,  // every body wholly hidden
    Ghosted = 2, // every body wholly ghosted
    Mixed = 3,   // some of it hidden or ghosted
};

/// One state per tree node of a document, in node order.
std::vector<NodeVisibility> nodeStates(const scene::Scene& scene, const FaceState& state,
                                       u32 document);

} // namespace geo::model
