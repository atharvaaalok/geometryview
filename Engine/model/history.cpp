#include "model/history.h"

namespace geo::model {
namespace {

usize sizeOf(const std::vector<Change>& changes) {
    return changes.capacity() * sizeof(Change);
}

// The flags each target has now, for the opposite direction.
std::vector<Change> currentFlags(const FaceState& state, const std::vector<Change>& changes) {
    std::vector<Change> current;
    current.reserve(changes.size());
    for (const Change& change : changes) {
        const Target target = change.target;
        const u8 own = target.face == Target::kWholeOccurrence
                           ? u8(state.occurrenceWords()[target.occurrence].flags & 0x7)
                           : state.faceBytes()[state.occurrenceWords()[target.occurrence].facePage +
                                               target.face];
        current.push_back({target, own});
    }
    return current;
}

} // namespace

void History::record(std::vector<Change> step) {
    if (step.empty()) {
        return;
    }
    for (const Step& old : redo_) {
        bytes_ -= sizeOf(old.before) + sizeOf(old.after);
    }
    redo_.clear();
    bytes_ += sizeOf(step);
    undo_.push_back({std::move(step), {}});
    trim();
}

bool History::undo(FaceState& state) {
    if (undo_.empty()) {
        return false;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    step.after = currentFlags(state, step.before);
    bytes_ += sizeOf(step.after);
    state.restore(step.before);
    redo_.push_back(std::move(step));
    return true;
}

bool History::redo(FaceState& state) {
    if (redo_.empty()) {
        return false;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    state.restore(step.after);
    bytes_ -= sizeOf(step.after);
    step.after.clear();
    step.after.shrink_to_fit();
    undo_.push_back(std::move(step));
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
}

void History::trim() {
    while (bytes_ > maxBytes_ && undo_.size() > 1) {
        bytes_ -= sizeOf(undo_.front().before) + sizeOf(undo_.front().after);
        undo_.pop_front();
    }
}

} // namespace geo::model
