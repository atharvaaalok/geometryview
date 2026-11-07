#include "cache/blob_writer.h"
#include "model/interaction.h"
#include "scene/scene.h"

#include <doctest/doctest.h>
#include <unistd.h>

using namespace geo;
using namespace geo::model;
using format::NodeKind;

namespace {

format::NodeRecord node(u32 parent, u32 end, u32 first, u32 last, NodeKind kind) {
    return {parent, end, first, last, 0, 0, u32(kind), 0};
}

// rig: part A with two bodies (3 and 5 faces), and part B with one body (3 faces).
// Pick ids: occurrence 0 is 1-3, occurrence 1 is 4-8, occurrence 2 is 9-11.
struct Rig {
    scene::Scene scene;
    Interaction interaction{scene};

    Rig() {
        import::ModelData model;
        model.definitions = {{{0, 0, 0}, {1, 1, 1}, 3, 0}, {{0, 0, 0}, {1, 1, 1}, 5, 0}};
        model.occurrences = {{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 0, 2, 0, 0},
                             {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 1, 3, 0, 0},
                             {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 0, 4, 0, 0}};
        model.nodes = {node(format::kNoParent, 5, 0, 3, NodeKind::Assembly),
                       node(0, 4, 0, 2, NodeKind::Part), node(1, 3, 0, 1, NodeKind::Body),
                       node(1, 4, 1, 2, NodeKind::Body), node(0, 5, 2, 3, NodeKind::Part)};

        char path[] = "/tmp/geomodelXXXXXX";
        const int fd = mkstemp(path);
        REQUIRE(fd >= 0);
        import::BlobStream stream(fd, nullptr);
        REQUIRE(stream.writeModel(model));
        REQUIRE(stream.finish({}));
        std::vector<std::shared_ptr<scene::MappedSegment>> segments;
        std::string error;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        unlink(path);
        scene::Document& document = scene.document(scene.addDocument());
        for (auto& segment : segments) {
            REQUIRE(document.add(std::move(segment), error));
        }
        interaction.sceneChanged();
    }

    u8 flags(u32 occurrence, u32 face = Target::kWholeOccurrence) const {
        return interaction.state().flags({occurrence, face});
    }
};

} // namespace

TEST_CASE("pick ids resolve to an occurrence and face") {
    Rig rig;
    const auto pick = rig.scene.resolve(5);
    REQUIRE(pick.has_value());
    CHECK(pick->occurrence == 1);
    CHECK(pick->face == 1);
    CHECK(rig.scene.resolve(11)->occurrence == 2);
    CHECK_FALSE(rig.scene.resolve(0).has_value());
    CHECK_FALSE(rig.scene.resolve(12).has_value());
}

TEST_CASE("hover covers a face, a body or a part") {
    Rig rig;
    CHECK(rig.interaction.hoverRange(5) == std::pair<u32, u32>(5, 6));
    rig.interaction.setMode(SelectionMode::Body);
    CHECK(rig.interaction.hoverRange(5) == std::pair<u32, u32>(4, 9));
    rig.interaction.setMode(SelectionMode::Part);
    CHECK(rig.interaction.hoverRange(5) == std::pair<u32, u32>(1, 9));
    CHECK(rig.interaction.hoverRange(10) == std::pair<u32, u32>(9, 12));
    CHECK(rig.interaction.hoverRange(0) == std::pair<u32, u32>(0, 0));
}

TEST_CASE("a click selects, an extended click toggles, and clicking nothing clears") {
    Rig rig;
    rig.interaction.click(5, Tool::Select, false);
    CHECK((rig.flags(1, 1) & kStateSelected) != 0);
    CHECK((rig.flags(1, 2) & kStateSelected) == 0);
    rig.interaction.click(6, Tool::Select, true);
    CHECK((rig.flags(1, 2) & kStateSelected) != 0);
    rig.interaction.click(6, Tool::Select, true);
    CHECK((rig.flags(1, 2) & kStateSelected) == 0);
    REQUIRE(rig.interaction.focus().has_value());
    CHECK(rig.interaction.focus()->node == 1);
    rig.interaction.click(0, Tool::Select, false);
    CHECK((rig.flags(1, 1) & kStateSelected) == 0);
}

TEST_CASE("hiding a part hides all its bodies and can be undone") {
    Rig rig;
    rig.interaction.setMode(SelectionMode::Part);
    rig.interaction.click(2, Tool::Hide, false);
    CHECK((rig.flags(0) & kStateHidden) != 0);
    CHECK((rig.flags(1) & kStateHidden) != 0);
    CHECK((rig.flags(2) & kStateHidden) == 0);
    REQUIRE(rig.interaction.undo());
    CHECK((rig.flags(0) & kStateHidden) == 0);
    REQUIRE(rig.interaction.redo());
    CHECK((rig.flags(1) & kStateHidden) != 0);
    rig.interaction.showAll();
    CHECK((rig.flags(1) & kStateHidden) == 0);
}

TEST_CASE("clicking a ghost with the ghost tool brings it back") {
    Rig rig;
    rig.interaction.click(9, Tool::Ghost, false);
    CHECK((rig.flags(2, 0) & kStateGhost) != 0);
    rig.interaction.click(9, Tool::Ghost, false);
    CHECK((rig.flags(2, 0) & kStateGhost) == 0);
}

TEST_CASE("isolate hides every occurrence with nothing selected") {
    Rig rig;
    rig.interaction.click(10, Tool::Select, false);
    rig.interaction.isolate();
    CHECK((rig.flags(0) & kStateHidden) != 0);
    CHECK((rig.flags(1) & kStateHidden) != 0);
    CHECK((rig.flags(2) & kStateHidden) == 0);
}

TEST_CASE("selecting a tree node selects everything beneath it") {
    Rig rig;
    rig.interaction.selectNode({0, 0}, false);
    for (u32 o = 0; o < 3; ++o) {
        CHECK((rig.flags(o) & kStateSelected) != 0);
    }
    rig.interaction.hideSelection();
    CHECK((rig.flags(2) & kStateHidden) != 0);
    CHECK(rig.interaction.nodeRange({0, 1}) == std::pair<u32, u32>(1, 9));
}
