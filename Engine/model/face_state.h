// What is hidden, ghosted and selected, per occurrence and per face, laid out for the GPU.
//
// Two levels keep this small: one word per occurrence, plus a page of one byte per face only for
// occurrences that have a face-level change. Every other occurrence points at a shared page of
// zeros. Hiding a part, however many faces it has, writes one word.
#pragma once

#include "base/types.h"
#include "shaders/shared/gpu_types.h"

#include <span>
#include <vector>

namespace geo::model {

/// Where a change applies: a whole occurrence, or one of its faces.
struct Target {
    u32 occurrence;
    u32 face; // kWholeOccurrence for all of it

    static constexpr u32 kWholeOccurrence = 0xFFFFFFFF;
    bool operator==(const Target&) const = default;
};

/// A change to undo, with the flags it replaced.
struct Change {
    Target target;
    u8 before;
};

class FaceState {
public:
    /// Sizes the state for `faceCounts`, one per occurrence across all documents. Existing state is
    /// kept for occurrences that already exist, so adding a file does not reset the others.
    void resize(std::span<const u32> faceCounts);

    u32 occurrenceCount() const { return u32(words_.size()); }

    /// Sets `mask` bits to `value` on a target. Changes to `kHistoryFlags` are appended to
    /// `changes`, if given, so they can be undone.
    void set(Target target, u8 mask, bool value, std::vector<Change>* changes = nullptr);

    /// Clears `mask` everywhere, recording what it changed.
    void clear(u8 mask, std::vector<Change>* changes = nullptr);
    /// Clears `mask` from an occurrence and each of its faces.
    void clearOccurrence(u32 occurrence, u8 mask, std::vector<Change>* changes = nullptr);

    /// Puts back the flags recorded in `changes`, newest first.
    void restore(std::span<const Change> changes);

    /// Flags of a target. A face's flags include its occurrence's.
    u8 flags(Target target) const;

    /// True if any face of the occurrence has `mask` set itself, or the occurrence does.
    bool anyFace(u32 occurrence, u8 mask) const;

    /// The GPU's view: a word per occurrence, then per-face bytes. Both change in place, and
    /// `version` changes with them so the renderer knows when to upload. Versions come from one
    /// counter for every face state, so a new one, as a file opened after another starts, never
    /// repeats a version an earlier one had and is never taken for it.
    std::span<const OccurrenceState> occurrenceWords() const { return words_; }
    std::span<const u8> faceBytes() const { return faceBytes_; }
    u64 version() const { return version_; }
    /// Counts changes to what is hidden or ghosted only, so selecting costs no visibility work.
    u64 visibilityVersion() const { return visibilityVersion_; }
    /// Counts changes to what is selected only.
    u64 selectionVersion() const { return selectionVersion_; }

private:
    static u64 nextVersion();
    u32 pageFor(u32 occurrence); // allocates one if needed
    u32 alignedEnd() const;

    std::vector<OccurrenceState> words_;
    std::vector<u32> faceCounts_;
    std::vector<u8> faceBytes_; // pages, including the shared zero page
    std::vector<u32> marks_;    // per occurrence and flag, how many faces have it themselves
    u32 zeroPage_ = 0;
    u32 zeroPageSize_ = 0;
    u64 version_ = nextVersion();
    u64 visibilityVersion_ = 0;
    u64 selectionVersion_ = 0;
};

} // namespace geo::model
