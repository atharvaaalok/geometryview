// Undo and redo for hiding and ghosting. Each step stores only the flags it replaced, so the cost
// follows what changed rather than the size of the model.
#pragma once

#include "model/face_state.h"

#include <deque>
#include <vector>

namespace geo::model {

class History {
public:
    explicit History(usize maxBytes = usize(64) << 20) : maxBytes_(maxBytes) {}

    /// Records a finished step. An empty one is ignored. Recording clears what could be redone.
    void record(std::vector<Change> step);

    /// Reverses the newest step on `state`. Returns false if there is nothing to undo.
    bool undo(FaceState& state);
    bool redo(FaceState& state);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void clear();

private:
    struct Step {
        std::vector<Change> before; // flags each target had before the step
        std::vector<Change> after;  // filled when the step is undone, for redo
    };

    void trim();

    std::deque<Step> undo_;
    std::vector<Step> redo_;
    usize bytes_ = 0;
    usize maxBytes_;
};

} // namespace geo::model
