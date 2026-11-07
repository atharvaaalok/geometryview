#include "base/time.h"
#include "model/face_state.h"
#include "model/history.h"

#include <doctest/doctest.h>
#include <format>
#include <vector>

using namespace geo;
using namespace geo::model;

namespace {

constexpr u32 kWhole = Target::kWholeOccurrence;

} // namespace

TEST_CASE("occurrences start clear and share one zero page") {
    FaceState state;
    const std::vector<u32> faces = {10, 20, 5};
    state.resize(faces);
    REQUIRE(state.occurrenceCount() == 3);
    for (const OccurrenceState& word : state.occurrenceWords()) {
        CHECK(word.flags == 0);
        CHECK(word.facePage == state.occurrenceWords()[0].facePage);
    }
    CHECK(state.faceBytes().size() >= 20);
}

TEST_CASE("a face flag gets its own page and marks its occurrence") {
    FaceState state;
    const std::vector<u32> faces = {10, 20};
    state.resize(faces);
    state.set({1, 7}, kStateHidden, true);
    CHECK(state.flags({1, 7}) == kStateHidden);
    CHECK(state.flags({1, 6}) == 0);
    CHECK(state.flags({0, 7}) == 0); // the zero page is untouched
    CHECK((state.occurrenceWords()[1].flags & kStateFaceHidden) != 0);
    CHECK(state.occurrenceWords()[1].facePage != state.occurrenceWords()[0].facePage);
    CHECK(state.anyFace(1, kStateHidden));
    CHECK_FALSE(state.anyFace(0, kStateHidden));

    state.set({1, 7}, kStateHidden, false);
    CHECK((state.occurrenceWords()[1].flags & kStateFaceHidden) == 0);
}

TEST_CASE("an occurrence flag applies to all of its faces") {
    FaceState state;
    const std::vector<u32> faces = {4};
    state.resize(faces);
    state.set({0, kWhole}, kStateGhost, true);
    CHECK(state.flags({0, 3}) == kStateGhost);
}

TEST_CASE("undo and redo restore exactly what changed") {
    FaceState state;
    const std::vector<u32> faces = {4, 4};
    state.resize(faces);
    History history;

    std::vector<Change> step;
    state.set({0, 1}, kStateHidden, true, &step);
    state.set({1, kWhole}, kStateHidden, true, &step);
    history.record(std::move(step));

    std::vector<Change> second;
    state.set({0, 2}, kStateGhost, true, &second);
    history.record(std::move(second));

    REQUIRE(history.undo(state));
    CHECK(state.flags({0, 2}) == 0);
    CHECK(state.flags({0, 1}) == kStateHidden);
    REQUIRE(history.undo(state));
    CHECK(state.flags({0, 1}) == 0);
    CHECK(state.flags({1, 0}) == 0);
    CHECK_FALSE(history.undo(state));

    REQUIRE(history.redo(state));
    CHECK(state.flags({0, 1}) == kStateHidden);
    CHECK(state.flags({1, 3}) == kStateHidden);
    REQUIRE(history.redo(state));
    CHECK(state.flags({0, 2}) == kStateGhost);
    CHECK_FALSE(history.redo(state));
}

TEST_CASE("showing all clears face pages and occurrences, and can be undone") {
    FaceState state;
    const std::vector<u32> faces = {8, 8};
    state.resize(faces);
    state.set({0, 3}, kStateHidden, true);
    state.set({1, kWhole}, kStateHidden, true);
    History history;
    std::vector<Change> step;
    state.clear(kStateHidden, &step);
    history.record(std::move(step));
    CHECK(state.flags({0, 3}) == 0);
    CHECK(state.flags({1, 0}) == 0);
    CHECK_FALSE(state.anyFace(0, kStateHidden));
    REQUIRE(history.undo(state));
    CHECK(state.flags({0, 3}) == kStateHidden);
    CHECK(state.flags({1, 0}) == kStateHidden);
}

TEST_CASE("selection is not touched by clearing hidden faces") {
    FaceState state;
    const std::vector<u32> faces = {4};
    state.resize(faces);
    state.set({0, 1}, kStateHidden | kStateSelected, true);
    state.clear(kStateHidden);
    CHECK(state.flags({0, 1}) == kStateSelected);
}

TEST_CASE("adding occurrences keeps existing state") {
    FaceState state;
    std::vector<u32> faces = {4};
    state.resize(faces);
    state.set({0, 2}, kStateGhost, true);
    faces = {4, 100};
    state.resize(faces);
    CHECK(state.flags({0, 2}) == kStateGhost);
    CHECK(state.flags({1, 99}) == 0);
    CHECK(state.occurrenceWords()[1].facePage != state.occurrenceWords()[0].facePage);
}

TEST_CASE("hiding a part of a million faces and undoing it takes well under a millisecond") {
    // A part is a range of occurrences, so hiding it writes one word per occurrence.
    FaceState state;
    const std::vector<u32> faces(1000, 1000); // a million faces in a thousand occurrences
    state.resize(faces);
    History history;
    const double start = nowSeconds();
    std::vector<Change> step;
    for (u32 o = 0; o < 1000; ++o) {
        state.set({o, kWhole}, kStateHidden, true, &step);
    }
    history.record(std::move(step));
    REQUIRE(history.undo(state));
    const double elapsed = nowSeconds() - start;
    MESSAGE(std::format("hid and restored a million faces in {:.3f} ms", elapsed * 1e3));
#if defined(NDEBUG) && !GEO_SANITIZED
    // Timing means something only in an optimised build without sanitizers.
    CHECK(elapsed < 0.001);
#endif
    CHECK(state.flags({999, 999}) == 0);
}
