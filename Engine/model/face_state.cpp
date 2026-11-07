#include "model/face_state.h"

#include <algorithm>

namespace geo::model {
namespace {

constexpr u8 kFaceFlags = kStateHidden | kStateGhost | kStateSelected;
constexpr u8 kVisibilityFlags = kStateHidden | kStateGhost;
constexpr u32 kFlagCount = 3;

// The occurrence bit saying some face has `flag` of its own.
u32 summaryOf(u32 bit) {
    return 1u << (bit + 4);
}

} // namespace

void FaceState::resize(std::span<const u32> faceCounts) {
    const usize previous = words_.size();
    const u32 largest =
        faceCounts.empty() ? 0 : *std::max_element(faceCounts.begin(), faceCounts.end());
    if (previous == 0 || largest > zeroPageSize_) {
        // A new, larger zero page goes at the end, so pages already handed out never move.
        const u32 page = alignedEnd();
        faceBytes_.resize(page + largest, 0);
        for (usize o = 0; o < previous; ++o) {
            if (words_[o].facePage == zeroPage_) {
                words_[o].facePage = page;
            }
        }
        zeroPage_ = page;
        zeroPageSize_ = largest;
    }
    words_.resize(faceCounts.size(), OccurrenceState{0, zeroPage_});
    faceCounts_.assign(faceCounts.begin(), faceCounts.end());
    marks_.resize(faceCounts.size() * kFlagCount, 0);
    ++version_;
}

u32 FaceState::alignedEnd() const {
    // Pages are four-byte aligned, so a shader could read them as words.
    return u32((faceBytes_.size() + 3) & ~usize(3));
}

u32 FaceState::pageFor(u32 occurrence) {
    OccurrenceState& word = words_[occurrence];
    if (word.facePage == zeroPage_) {
        const u32 page = alignedEnd();
        faceBytes_.resize(page + faceCounts_[occurrence], 0);
        word.facePage = page;
    }
    return word.facePage;
}

u8 FaceState::flags(Target target) const {
    const OccurrenceState& word = words_[target.occurrence];
    const u8 own = u8(word.flags & kFaceFlags);
    if (target.face == Target::kWholeOccurrence) {
        return own;
    }
    return u8(own | faceBytes_[word.facePage + target.face]);
}

bool FaceState::anyFace(u32 occurrence, u8 mask) const {
    const u32 flags = words_[occurrence].flags;
    for (u32 bit = 0; bit < kFlagCount; ++bit) {
        if ((mask & (1u << bit)) != 0 && (flags & ((1u << bit) | summaryOf(bit))) != 0) {
            return true;
        }
    }
    return false;
}

void FaceState::set(Target target, u8 mask, bool value, std::vector<Change>* changes) {
    mask &= kFaceFlags;
    OccurrenceState& word = words_[target.occurrence];
    if (target.face == Target::kWholeOccurrence) {
        const u8 before = u8(word.flags & kFaceFlags);
        const u8 after = value ? u8(before | mask) : u8(before & ~mask);
        if (after != before) {
            if (changes != nullptr) {
                changes->push_back({target, before});
            }
            word.flags = (word.flags & ~u32(kFaceFlags)) | after;
            ++version_;
            visibilityVersion_ += ((before ^ after) & kVisibilityFlags) != 0 ? 1 : 0;
            selectionVersion_ += ((before ^ after) & kStateSelected) != 0 ? 1 : 0;
        }
        return;
    }
    if (target.face >= faceCounts_[target.occurrence]) {
        return;
    }
    const u8 before = faceBytes_[word.facePage + target.face];
    const u8 after = value ? u8(before | mask) : u8(before & ~mask);
    if (after == before) {
        return;
    }
    if (changes != nullptr) {
        changes->push_back({target, before});
    }
    faceBytes_[pageFor(target.occurrence) + target.face] = after;
    OccurrenceState& updated = words_[target.occurrence];
    for (u32 bit = 0; bit < kFlagCount; ++bit) {
        const u8 flag = u8(1u << bit);
        if (((before ^ after) & flag) == 0) {
            continue;
        }
        u32& marks = marks_[usize(target.occurrence) * kFlagCount + bit];
        marks = (after & flag) != 0 ? marks + 1 : marks - 1;
        updated.flags =
            marks != 0 ? updated.flags | summaryOf(bit) : updated.flags & ~summaryOf(bit);
    }
    ++version_;
    visibilityVersion_ += ((before ^ after) & kVisibilityFlags) != 0 ? 1 : 0;
    selectionVersion_ += ((before ^ after) & kStateSelected) != 0 ? 1 : 0;
}

void FaceState::clear(u8 mask, std::vector<Change>* changes) {
    for (u32 o = 0; o < words_.size(); ++o) {
        clearOccurrence(o, mask, changes);
    }
}

void FaceState::clearOccurrence(u32 o, u8 mask, std::vector<Change>* changes) {
    mask &= kFaceFlags;
    if ((words_[o].flags & mask) != 0) {
        set({o, Target::kWholeOccurrence}, mask, false, changes);
    }
    bool marked = false;
    for (u32 bit = 0; bit < kFlagCount; ++bit) {
        marked |= (mask & (1u << bit)) != 0 && marks_[usize(o) * kFlagCount + bit] != 0;
    }
    if (!marked) {
        return;
    }
    const u32 page = words_[o].facePage;
    for (u32 f = 0; f < faceCounts_[o]; ++f) {
        if ((faceBytes_[page + f] & mask) != 0) {
            set({o, f}, mask, false, changes);
        }
    }
}

void FaceState::restore(std::span<const Change> changes) {
    for (usize i = changes.size(); i-- > 0;) {
        const Change& change = changes[i];
        const Target target = change.target;
        const u8 own = target.face == Target::kWholeOccurrence
                           ? u8(words_[target.occurrence].flags & kFaceFlags)
                           : faceBytes_[words_[target.occurrence].facePage + target.face];
        set(target, u8(own & ~change.before), false);
        set(target, u8(change.before & ~own), true);
    }
}

} // namespace geo::model
