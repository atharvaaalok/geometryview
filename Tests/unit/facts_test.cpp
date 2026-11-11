// A file's facts: its box in millimetres, what it is made of and its name, from a cache file, and
// estimated from its structure before any geometry exists.
#include "scene/facts.h"

#include "drive_fixture.h"
#include "import.h"
#include "step/slicing.h"
#include "step_fixture.h"

#include <cstdlib>
#include <doctest/doctest.h>
#include <string>
#include <unistd.h>

using namespace geo;

namespace {

// A temporary cache file, removed afterwards.
struct TempBlob {
    std::string path;
    int fd = -1;
    TempBlob() {
        char pattern[] = "/tmp/geofactsXXXXXX";
        fd = mkstemp(pattern);
        REQUIRE(fd >= 0);
        path = pattern;
    }
    ~TempBlob() {
        close(fd);
        unlink(path.c_str());
    }
};

scene::Facts factsOf(const TempBlob& blob) {
    scene::Facts facts;
    std::string error;
    REQUIRE_MESSAGE(scene::measureCacheFile(blob.path, facts, error), error);
    return facts;
}

void checkDriveBox(const scene::Facts& facts) {
    REQUIRE(facts.hasBox);
    // The housing spans 40 × 30 from the origin; the winding rises to 6 + 20.
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(facts.min[axis] == doctest::Approx(0.0).epsilon(1e-4));
    }
    CHECK(facts.max[0] == doctest::Approx(40.0).epsilon(1e-4));
    CHECK(facts.max[1] == doctest::Approx(30.0).epsilon(1e-4));
    CHECK(facts.max[2] == doctest::Approx(26.0).epsilon(1e-4));
}

} // namespace

TEST_CASE("a mesh's facts are measured from its triangles") {
    const std::string stl = "solid bracket\n"
                            "facet normal 0 0 1\nouter loop\n"
                            "vertex 0 0 0\nvertex 12.5 0 0\nvertex 0 4 3\n"
                            "endloop\nendfacet\n"
                            "facet normal 0 0 1\nouter loop\n"
                            "vertex 12.5 0 0\nvertex 12.5 4 0\nvertex 0 4 3\n"
                            "endloop\nendfacet\nendsolid bracket\n";
    TempBlob blob;
    import::ImportResult result;
    std::string error;
    REQUIRE(import::importMesh({reinterpret_cast<const u8*>(stl.data()), stl.size()}, blob.fd,
                               nullptr, result, error));
    const scene::Facts facts = factsOf(blob);
    CHECK_FALSE(facts.estimated);
    CHECK(facts.triangles == 2);
    CHECK(facts.parts == 1);
    CHECK(facts.bodies == 1);
    CHECK(facts.size()[0] == doctest::Approx(12.5).epsilon(1e-4));
    CHECK(facts.size()[1] == doctest::Approx(4.0).epsilon(1e-4));
    CHECK(facts.size()[2] == doctest::Approx(3.0).epsilon(1e-4));
    CHECK(facts.hash.size() == 32);
}

TEST_CASE("a STEP file's structure gives its parts, its name and an estimated box") {
    const std::string text = test::driveStep();
    const std::span<const u8> file(reinterpret_cast<const u8*>(text.data()), text.size());
    TempBlob blob;
    import::StepImport import;
    import::StepOptions options;
    options.structureOnly = true;
    import::StepStats stats;
    std::string error;
    REQUIRE_MESSAGE(import.begin(file, blob.fd, nullptr, options, stats, error), error);
    REQUIRE_FALSE(import.needsWholeFile());
    REQUIRE_MESSAGE(import.finishStructure(error), error);

    const scene::Facts facts = factsOf(blob);
    CHECK(facts.estimated);
    CHECK(facts.triangles == 0);
    CHECK(facts.parts == 6);
    CHECK(facts.bodies == 6);
    CHECK(facts.definitions == 6);
    CHECK(facts.faces == 36);
    CHECK(facts.name == "drive");
    // Boxes estimate themselves exactly.
    checkDriveBox(facts);
}

#ifdef GEO_TEST_WORKER_PATH
TEST_CASE("an imported STEP file's facts are measured, no longer estimated") {
    const std::string path = test::importDrive(GEO_TEST_WORKER_PATH);
    scene::Facts facts;
    std::string error;
    REQUIRE_MESSAGE(scene::measureCacheFile(path, facts, error), error);
    unlink(path.c_str());
    CHECK_FALSE(facts.estimated);
    CHECK(facts.triangles == 6 * 12);
    CHECK(facts.parts == 6);
    CHECK(facts.name == "drive");
    checkDriveBox(facts);
}
#endif

TEST_CASE("facts read back what they write, and refuse another version's") {
    scene::Facts facts;
    facts.min = {-1.5, 0, 2};
    facts.max = {3, 4.25, 8};
    facts.hasBox = true;
    facts.estimated = false;
    facts.triangles = 123456789;
    facts.faces = 42;
    facts.parts = 3;
    facts.bodies = 5;
    facts.definitions = 4;
    facts.name = "Spindle \"B\"";
    facts.hash = "00ff";
    json::Value value;
    std::string error;
    REQUIRE(json::parse(facts.toJson().write(), value, error));
    scene::Facts back;
    REQUIRE(scene::Facts::fromJson(value, back));
    CHECK(back == facts);

    value.set("version", 0);
    CHECK_FALSE(scene::Facts::fromJson(value, back));

    // Without a box, none is read back.
    scene::Facts empty;
    REQUIRE(json::parse(empty.toJson().write(), value, error));
    REQUIRE(scene::Facts::fromJson(value, back));
    CHECK_FALSE(back.hasBox);
}
