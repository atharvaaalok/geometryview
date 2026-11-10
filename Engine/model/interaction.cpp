#include "model/interaction.h"

#include <algorithm>

namespace geo::model {

using format::NodeKind;
using format::NodeRecord;

void Interaction::sceneChanged() {
    std::vector<u32> faces = scene_.faceCounts();
    if (faces.size() != state_.occurrenceCount()) {
        state_.resize(faces);
        faceCounts_ = std::move(faces);
    }
}

void Interaction::reset() {
    state_ = FaceState();
    faceCounts_.clear();
    history_.clear();
    focus_.reset();
}

std::optional<Interaction::Located> Interaction::locate(u32 id) const {
    const std::optional<scene::PickTarget> pick = scene_.resolve(id);
    if (!pick) {
        return std::nullopt;
    }
    return Located{*pick, partOf(pick->document, pick->occurrence)};
}

// The part a body belongs to: its own row, unless it is one body among several in a part.
NodeRef Interaction::partOf(u32 document, u32 occurrence) const {
    const format::SegmentView& model = scene_.document(document).model();
    u32 node = model.occurrences[occurrence].node;
    while (model.nodes[node].kind == u32(NodeKind::Body) &&
           model.nodes[node].parent != format::kNoParent &&
           model.nodes[model.nodes[node].parent].kind == u32(NodeKind::Part)) {
        node = model.nodes[node].parent;
    }
    return {document, node};
}

void Interaction::occurrencesOf(NodeRef node, std::vector<Target>& out) const {
    const NodeRecord& record = scene_.document(node.document).model().nodes[node.node];
    const u32 base = scene_.occurrenceBase(node.document);
    for (u32 o = record.firstOccurrence; o < record.occurrenceEnd; ++o) {
        out.push_back({base + o, Target::kWholeOccurrence});
    }
}

void Interaction::targetsFor(const Located& located, std::vector<Target>& out) const {
    out.clear();
    switch (mode_) {
    case SelectionMode::Face:
        out.push_back({located.pick.globalOccurrence, located.pick.face});
        break;
    case SelectionMode::Body:
        out.push_back({located.pick.globalOccurrence, Target::kWholeOccurrence});
        break;
    case SelectionMode::Part:
        occurrencesOf(located.part, out);
        break;
    }
}

std::pair<u32, u32> Interaction::nodeRange(NodeRef node) const {
    const NodeRecord& record = scene_.document(node.document).model().nodes[node.node];
    if (record.firstOccurrence == record.occurrenceEnd) {
        return {0, 0};
    }
    // Occurrences are numbered depth first, so a node's are consecutive, and so are their ids.
    return {scene_.idRange(node.document, record.firstOccurrence).first,
            scene_.idRange(node.document, record.occurrenceEnd - 1).second};
}

std::pair<u32, u32> Interaction::hoverRange(u32 id) const {
    const std::optional<Located> located = locate(id);
    if (!located) {
        return {0, 0};
    }
    switch (mode_) {
    case SelectionMode::Face:
        return {id, id + 1};
    case SelectionMode::Body:
        return scene_.idRange(located->pick.document, located->pick.occurrence);
    case SelectionMode::Part:
        return nodeRange(located->part);
    }
    return {0, 0};
}

void Interaction::apply(const std::vector<Target>& targets, u8 flag, bool value, bool recorded) {
    std::vector<Change> changes;
    for (const Target& target : targets) {
        state_.set(target, flag, value, recorded ? &changes : nullptr);
    }
    if (recorded) {
        history_.record(std::move(changes));
    }
}

bool Interaction::click(u32 id, Tool tool, bool extend) {
    const u64 before = state_.version();
    const std::optional<Located> located = locate(id);
    if (!located) {
        if (tool == Tool::Select && !extend) {
            clearSelection();
        }
        return state_.version() != before;
    }
    targetsFor(*located, scratch_);
    switch (tool) {
    case Tool::Select: {
        const bool allSelected =
            std::all_of(scratch_.begin(), scratch_.end(), [&](const Target& target) {
                return (state_.flags(target) & kStateSelected) != 0;
            });
        if (!extend) {
            state_.clear(kStateSelected);
        }
        apply(scratch_, kStateSelected, !(extend && allSelected), false);
        focus_ = located->part;
        break;
    }
    case Tool::Hide:
        apply(scratch_, kStateHidden, true, true);
        break;
    case Tool::Ghost: {
        // Clicking a ghost, which only the ghost tool can hit, brings it back.
        const bool ghosted =
            (state_.flags({located->pick.globalOccurrence, located->pick.face}) & kStateGhost) != 0;
        apply(scratch_, kStateGhost, !ghosted, true);
        break;
    }
    }
    return state_.version() != before;
}

void Interaction::selectNode(NodeRef node, bool extend) {
    if (!extend) {
        state_.clear(kStateSelected);
    }
    scratch_.clear();
    occurrencesOf(node, scratch_);
    const bool allSelected =
        std::all_of(scratch_.begin(), scratch_.end(), [&](const Target& target) {
            return (state_.flags(target) & kStateSelected) != 0;
        });
    apply(scratch_, kStateSelected, !(extend && allSelected), false);
    focus_ = node;
}

void Interaction::clearSelection() {
    state_.clear(kStateSelected);
    focus_.reset();
}

// Visits every selected target: whole occurrences, or single faces of those with face selections.
void Interaction::forEachSelected(auto&& visit) const {
    const std::span<const OccurrenceState> words = state_.occurrenceWords();
    for (u32 o = 0; o < words.size(); ++o) {
        if ((words[o].flags & kStateSelected) != 0) {
            visit(Target{o, Target::kWholeOccurrence});
        } else if ((words[o].flags & kStateFaceSelected) != 0) {
            const u32 faces = faceCounts_[o];
            for (u32 f = 0; f < faces; ++f) {
                if ((state_.faceBytes()[words[o].facePage + f] & kStateSelected) != 0) {
                    visit(Target{o, f});
                }
            }
        }
    }
}

void Interaction::hideSelection() {
    scratch_.clear();
    forEachSelected([&](Target target) { scratch_.push_back(target); });
    apply(scratch_, kStateHidden, true, true);
}

void Interaction::ghostSelection() {
    scratch_.clear();
    forEachSelected([&](Target target) { scratch_.push_back(target); });
    apply(scratch_, kStateGhost, true, true);
}

// Everything comes back, hidden and ghosted alike, in one step to undo.
void Interaction::showAll() {
    std::vector<Change> changes;
    state_.clear(kStateHidden | kStateGhost, &changes);
    history_.record(std::move(changes));
}

void Interaction::unghostAll() {
    std::vector<Change> changes;
    state_.clear(kStateGhost, &changes);
    history_.record(std::move(changes));
}

void Interaction::isolate() {
    std::vector<u8> keep(state_.occurrenceCount(), 0);
    for (u32 o = 0; o < keep.size(); ++o) {
        keep[o] = state_.anyFace(o, kStateSelected) ? 1 : 0;
    }
    isolateKeeping(keep);
}

void Interaction::isolateNode(NodeRef node) {
    std::vector<u8> keep(state_.occurrenceCount(), 0);
    scratch_.clear();
    occurrencesOf(node, scratch_);
    for (const Target& target : scratch_) {
        keep[target.occurrence] = 1;
    }
    isolateKeeping(keep);
}

// One step: what is kept is shown and unghosted, faces included, and everything else is hidden.
void Interaction::isolateKeeping(const std::vector<u8>& keep) {
    if (std::find(keep.begin(), keep.end(), 1) == keep.end()) {
        return;
    }
    std::vector<Change> changes;
    for (u32 o = 0; o < keep.size(); ++o) {
        if (keep[o] != 0) {
            state_.clearOccurrence(o, kStateHidden | kStateGhost, &changes);
        } else {
            state_.set({o, Target::kWholeOccurrence}, kStateHidden, true, &changes);
        }
    }
    history_.record(std::move(changes));
}

void Interaction::setNode(NodeRef node, u8 flag, bool value) {
    scratch_.clear();
    occurrencesOf(node, scratch_);
    std::vector<Change> changes;
    for (const Target& target : scratch_) {
        if (value) {
            state_.set(target, flag, true, &changes);
        } else {
            state_.clearOccurrence(target.occurrence, flag, &changes);
        }
    }
    history_.record(std::move(changes));
}

std::optional<std::pair<NodeRef, u32>> Interaction::partAt(u32 id) const {
    const std::optional<Located> located = locate(id);
    if (!located) {
        return std::nullopt;
    }
    return std::pair(located->part, located->pick.face);
}

std::pair<u32, u32> Interaction::selectionCounts() const {
    u32 occurrences = 0, faces = 0;
    const std::span<const OccurrenceState> words = state_.occurrenceWords();
    for (u32 o = 0; o < words.size(); ++o) {
        if (!state_.anyFace(o, kStateSelected)) {
            continue;
        }
        ++occurrences;
        if ((words[o].flags & kStateSelected) != 0) {
            faces += faceCounts_[o];
            continue;
        }
        for (u32 f = 0; f < faceCounts_[o]; ++f) {
            faces += (state_.faceBytes()[words[o].facePage + f] & kStateSelected) != 0 ? 1 : 0;
        }
    }
    return {occurrences, faces};
}

bool Interaction::undo() {
    return history_.undo(state_);
}

bool Interaction::redo() {
    return history_.redo(state_);
}

} // namespace geo::model
