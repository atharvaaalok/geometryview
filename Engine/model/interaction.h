// What clicks and commands do to the model: select, hide and ghost at the chosen granularity, and
// undo. Kept apart from the engine so it can be tested without a GPU.
#pragma once

#include "model/face_state.h"
#include "model/history.h"
#include "scene/scene.h"

#include <optional>
#include <utility>
#include <vector>

namespace geo::model {

enum class SelectionMode : u8 { Face, Body, Part };

/// What a click does, chosen by a held key.
enum class Tool : u8 { Select, Hide, Ghost };

/// A tree node, for selecting or revealing it.
struct NodeRef {
    u32 document;
    u32 node;
    bool operator==(const NodeRef&) const = default;
};

class Interaction {
public:
    explicit Interaction(const scene::Scene& scene) : scene_(scene) {}

    /// Call when documents or their models change. State for existing occurrences is kept.
    void sceneChanged();
    /// Call when the scene is emptied, as when a new file replaces it.
    void reset();

    void setMode(SelectionMode mode) { mode_ = mode; }
    SelectionMode mode() const { return mode_; }

    const FaceState& state() const { return state_; }

    /// The ids that light up when the pointer is over `id`: the face, its body or its part.
    std::pair<u32, u32> hoverRange(u32 id) const;
    /// The ids of everything under a tree node.
    std::pair<u32, u32> nodeRange(NodeRef node) const;

    /// Applies a click on `id`, or on nothing when it is 0. With `extend`, selection toggles
    /// instead of replacing. Returns true if anything changed.
    bool click(u32 id, Tool tool, bool extend);
    void selectNode(NodeRef node, bool extend);

    void clearSelection();
    void hideSelection();
    void ghostSelection();
    void showAll();
    void unghostAll();
    /// Hides every occurrence with nothing selected in it.
    void isolate();
    bool undo();
    bool redo();

    /// The tree node the last selection belongs to, for the tree to reveal.
    std::optional<NodeRef> focus() const { return focus_; }

    /// The part a pick id belongs to, and its face within its body.
    std::optional<std::pair<NodeRef, u32>> partAt(u32 id) const;

    /// Selected bodies, and faces counting every face of a wholly selected body.
    std::pair<u32, u32> selectionCounts() const;

private:
    struct Located {
        scene::PickTarget pick;
        NodeRef part;
    };

    std::optional<Located> locate(u32 id) const;
    NodeRef partOf(u32 document, u32 occurrence) const;
    void targetsFor(const Located& located, std::vector<Target>& out) const;
    void occurrencesOf(NodeRef node, std::vector<Target>& out) const;
    void apply(const std::vector<Target>& targets, u8 flag, bool value, bool recorded);
    void forEachSelected(auto&& visit) const;

    const scene::Scene& scene_;
    FaceState state_;
    History history_;
    SelectionMode mode_ = SelectionMode::Face;
    std::optional<NodeRef> focus_;
    std::vector<u32> faceCounts_;
    std::vector<Target> scratch_;
};

} // namespace geo::model
