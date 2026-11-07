#include "render/looks.h"

#include <string_view>
#include <unordered_map>

namespace geo::render {
namespace {

// Cycle positions repeat after this many, which keeps both the six swatches and metal's four plain
// finishes in step while bounding the number of distinct looks.
constexpr u32 kCyclePeriod = 12;

u64 lookKey(const Look& look) {
    return u64(look.kind) | u64(look.material) << 8 | u64(look.byGroup) << 16 |
           u64(look.index) << 24 | u64(look.color) << 32;
}

class LookTable {
public:
    u16 add(const Look& look) {
        const auto [it, inserted] = indices_.try_emplace(lookKey(look), u16(looks_.size()));
        if (inserted) {
            if (looks_.size() == kMaxLooks) {
                indices_.erase(it);
                return 0; // a scene with this many file colours shares the first look
            }
            looks_.push_back(look);
        }
        return it->second;
    }
    std::vector<Look> take() { return std::move(looks_); }

private:
    std::unordered_map<u64, u16> indices_;
    std::vector<Look> looks_;
};

std::string_view nameOf(const format::SegmentView& model, u32 node) {
    const format::NodeRecord& record = model.nodes[node];
    return {model.strings.data() + record.nameOffset, record.nameLength};
}

// The part a node belongs to: itself, or the part above a body.
u32 partOf(const format::SegmentView& model, u32 node) {
    const format::NodeRecord& record = model.nodes[node];
    if (record.kind == u32(format::NodeKind::Body) && record.parent != format::kNoParent) {
        return record.parent;
    }
    return node;
}

// The top-level sub-assembly holding a node: the child of the root on its way up.
u32 groupOf(const format::SegmentView& model, u32 node) {
    u32 at = node;
    while (model.nodes[at].parent != format::kNoParent &&
           model.nodes[model.nodes[at].parent].parent != format::kNoParent) {
        at = model.nodes[at].parent;
    }
    return at;
}

} // namespace

LookAssignment assignLooks(const scene::Scene& scene, ColorMode mode) {
    LookAssignment out;
    out.ofOccurrence.reserve(scene.occurrenceCount());
    LookTable table;
    // Group and part numbers count across documents, so two files shown together differ.
    u32 groups = 0, parts = 0;
    for (u32 d = 0; d < scene.documentCount(); ++d) {
        const scene::Document& document = scene.document(d);
        if (!document.hasModel()) {
            continue;
        }
        const format::SegmentView& model = document.model();
        // Names repeat across instances, so each node is read once.
        std::vector<i32> materialOfNode(model.nodes.size(), -2); // -2 unread, -1 none
        std::vector<u32> numberOfNode(model.nodes.size(), ~0u);
        const auto materialOf = [&](u32 node) -> i32 {
            if (materialOfNode[node] == -2) {
                const std::optional<MaterialClass> m = materialFromName(nameOf(model, node));
                materialOfNode[node] = m ? i32(*m) : -1;
            }
            return materialOfNode[node];
        };
        const auto numberOf = [&](u32 node, u32& counter) {
            if (numberOfNode[node] == ~0u) {
                numberOfNode[node] = counter++;
            }
            return numberOfNode[node];
        };

        for (const format::OccurrenceRecord& occurrence : model.occurrences) {
            const u32 node = occurrence.node;
            Look look;
            switch (mode) {
            case ColorMode::Mono:
                break;
            case ColorMode::File:
                if (occurrence.color != 0) {
                    look.kind = Look::Kind::Color;
                    look.color = occurrence.color | 0xFF000000u;
                }
                break;
            case ColorMode::Parts:
                look.kind = Look::Kind::Cycle;
                look.index = numberOf(partOf(model, node), parts) % kCyclePeriod;
                break;
            case ColorMode::Material: {
                i32 material = materialOf(node);
                if (material < 0) {
                    material = materialOf(partOf(model, node));
                }
                if (material >= 0) {
                    look.kind = Look::Kind::Material;
                    look.material = MaterialClass(material);
                } else {
                    look.kind = Look::Kind::Cycle;
                    look.byGroup = true;
                    look.index = numberOf(groupOf(model, node), groups) % kCyclePeriod;
                }
                break;
            }
            }
            out.ofOccurrence.push_back(table.add(look));
        }
    }
    out.looks = table.take();
    if (out.looks.empty()) {
        out.looks.push_back(Look{});
    }
    return out;
}

std::vector<u32> paletteTexels(const Style& style, std::span<const Look> looks) {
    std::vector<u32> texels;
    texels.reserve(looks.size() * 3);
    for (const Look& look : looks) {
        const std::array<u32, 3> t = bodyTexels(style, look);
        texels.insert(texels.end(), t.begin(), t.end());
    }
    return texels;
}

std::vector<u32> swatchColors(const Style& style, const LookAssignment& assignment) {
    // There are few looks and perhaps a million occurrences, so each look is coloured once.
    std::vector<u32> ofLook(assignment.looks.size());
    for (usize l = 0; l < ofLook.size(); ++l) {
        ofLook[l] = swatch(style, assignment.looks[l]);
    }
    std::vector<u32> colors(assignment.ofOccurrence.size());
    for (usize o = 0; o < colors.size(); ++o) {
        colors[o] = ofLook[assignment.ofOccurrence[o]];
    }
    return colors;
}

} // namespace geo::render
