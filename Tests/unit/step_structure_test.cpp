#include "step/definitions.h"
#include "step/part21.h"
#include "step/structure.h"
#include "step_fixture.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace geo;
using namespace geo::import::step;

namespace {

struct Indexed {
    std::string text;
    std::vector<u8> scratch;
    Part21 index;

    explicit Indexed(std::string file) : text(std::move(file)) {
        std::string error;
        REQUIRE(indexPart21({reinterpret_cast<const u8*>(text.data()), text.size()}, index, scratch,
                            error));
    }
};

std::string_view nameOf(const Structure& structure, u32 node) {
    const TreeNode& row = structure.nodes[node];
    return std::string_view(structure.names).substr(row.nameOffset, row.nameLength);
}

// An assembly of two identical boxes and one larger box, at known offsets.
std::string threeBoxes(const char* unit = "MILLI") {
    test::StepWriter writer(unit);
    const int root = writer.assembly("rig");
    const int smallA = writer.box({0, 0, 0}, {1, 2, 3});
    const int smallB = writer.box({0, 0, 0}, {1, 2, 3});
    const int large = writer.box({0, 0, 0}, {4, 4, 4});
    writer.place(root, writer.part("left", smallA), smallA, {10, 0, 0});
    writer.place(root, writer.part("right", smallB), smallB, {20, 0, 0});
    writer.place(root, writer.part("base", large), large, {0, 0, -5});
    return writer.text();
}

} // namespace

TEST_CASE("an assembly reads as a tree of placed bodies") {
    Indexed file(threeBoxes());
    const Structure structure = readStructure(file.index);
    REQUIRE_FALSE(structure.needsWholeFile);
    REQUIRE(structure.nodes.size() == 4); // single-body parts need no body rows
    CHECK(nameOf(structure, 0) == "rig");
    CHECK(structure.nodes[0].kind == NodeKind::Assembly);
    CHECK(structure.nodes[0].subtreeEnd == 4);
    CHECK(nameOf(structure, 1) == "left");
    CHECK(structure.nodes[1].kind == NodeKind::Part);
    CHECK(structure.nodes[1].parent == 0);

    REQUIRE(structure.occurrences.size() == 3);
    CHECK(structure.occurrences[0].world.translation.x == doctest::Approx(10));
    CHECK(structure.occurrences[1].world.translation.x == doctest::Approx(20));
    CHECK(structure.occurrences[2].world.translation.z == doctest::Approx(-5));
    CHECK(structure.occurrences[0].node == 1);
    CHECK(structure.bodies.size() == 3);
    CHECK(structure.bodies[0].faceCount == 6);
    CHECK(structure.bodies[0].closed);
}

TEST_CASE("placements and bounds convert to millimetres") {
    Indexed file(threeBoxes(""));
    const Structure structure = readStructure(file.index);
    REQUIRE(structure.occurrences.size() == 3);
    CHECK(structure.occurrences[0].world.translation.x == doctest::Approx(10000));
    const Definitions definitions = buildDefinitions(file.index, structure);
    const Definition& box = definitions.items[definitions.definitionOfBody[0]];
    CHECK(box.boundsMax[2] == doctest::Approx(3000));
}

TEST_CASE("identical bodies become one definition") {
    Indexed file(threeBoxes());
    const Structure structure = readStructure(file.index);
    const Definitions definitions = buildDefinitions(file.index, structure);
    REQUIRE(definitions.items.size() == 2);
    CHECK(definitions.definitionOfBody[0] == definitions.definitionOfBody[1]);
    CHECK(definitions.definitionOfBody[0] != definitions.definitionOfBody[2]);
    CHECK(definitions.items[definitions.definitionOfBody[0]].occurrenceCount == 2);
    CHECK(definitions.faces.size() == 12);
}

TEST_CASE("face order comes from the file, so it is the same on every read") {
    Indexed first(threeBoxes());
    Indexed second(threeBoxes());
    const Definitions a = buildDefinitions(first.index, readStructure(first.index));
    const Definitions b = buildDefinitions(second.index, readStructure(second.index));
    REQUIRE(a.faces.size() == b.faces.size());
    for (usize f = 0; f < a.faces.size(); ++f) {
        CHECK(a.faces[f].record == b.faces[f].record);
    }
}

TEST_CASE("a cavity is its own shell, reversed by the file") {
    test::StepWriter writer;
    const int representation = writer.box({0, 0, 0}, {4, 4, 4}, true);
    writer.part("hollow", representation);
    Indexed file(writer.text());
    const Structure structure = readStructure(file.index);
    REQUIRE(structure.bodies.size() == 1);
    const Body& body = structure.bodies[0];
    CHECK(body.hasVoids);
    CHECK(body.shellCount == 2);
    REQUIRE(body.faceCount == 12);
    for (u32 f = 0; f < 12; ++f) {
        const FaceUse& face = structure.faces[body.firstFace + f];
        CHECK(face.shell == (f < 6 ? 0 : 1));
        CHECK(face.flipped == (f >= 6));
    }
}

TEST_CASE("a slice is a STEP file holding exactly its faces") {
    Indexed file(threeBoxes());
    const Structure structure = readStructure(file.index);
    const Definitions definitions = buildDefinitions(file.index, structure);
    const Definition& box = definitions.items[0];
    std::vector<u32> faces;
    for (u32 f = 0; f < 3; ++f) {
        faces.push_back(definitions.faces[box.firstFace + f].record);
    }
    ClosureWalker walker(file.index);
    std::string slice;
    writeSlice(file.index, faces, structure.bodies[box.body].context, walker, slice);

    Indexed sliced(slice);
    u32 advancedFaces = 0, shells = 0;
    for (u32 r = 0; r < sliced.index.recordCount(); ++r) {
        advancedFaces += sliced.index.kinds[r] == Kind::ADVANCED_FACE;
        shells += sliced.index.kinds[r] == Kind::OPEN_SHELL;
        for (const u32 ref : sliced.index.refsOf(r)) {
            CHECK(ref != kNoRecord); // the closure is complete
        }
    }
    CHECK(advancedFaces == 3);
    CHECK(shells == 1);
}
