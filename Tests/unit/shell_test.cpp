#include "render/shell.h"

#include <doctest/doctest.h>

using namespace geo;
using namespace geo::render;

namespace {

Aabb box(double lo, double hi) {
    return {simd_make_double3(lo, lo, lo), simd_make_double3(hi, hi, hi)};
}

// A housing that hides a gear in most views.
ShellCounts housedGear() {
    ShellCounts counts(2);
    std::vector<u32> front(100, 1), behind(100, 0);
    for (int i = 0; i < 80; ++i) {
        behind[i] = 2;
    }
    for (int i = 80; i < 90; ++i) {
        front[i] = 2;
    }
    counts.accumulate(front, behind);
    return counts;
}

} // namespace

TEST_CASE("a housing around a hidden gear is shell, and the gear is not") {
    const std::vector<ShellPart> parts = {{"part_17", box(0, 10)}, {"part_18", box(2, 8)}};
    const std::vector<u8> shell = decideShell(housedGear(), parts, 0.3);
    CHECK(shell[0] == 1);
    CHECK(shell[1] == 0);
}

TEST_CASE("a part that does not enclose what it hides is not shell unless named so") {
    const std::vector<ShellPart> apart = {{"part_17", box(0, 10)}, {"part_18", box(20, 30)}};
    CHECK(decideShell(housedGear(), apart, 0.3)[0] == 0);
    const std::vector<ShellPart> named = {{"Cover", box(0, 10)}, {"part_18", box(20, 30)}};
    CHECK(decideShell(housedGear(), named, 0.3)[0] == 1);
}

TEST_CASE("the mechanism never fades, and when everything qualifies nothing does") {
    const std::vector<ShellPart> gear = {{"Gear ring", box(0, 10)}, {"part_18", box(2, 8)}};
    CHECK(decideShell(housedGear(), gear, 0.3)[0] == 0);

    ShellCounts lone(1);
    const std::vector<u32> front(10, 1), behind(10, 0);
    lone.accumulate(front, behind);
    const std::vector<ShellPart> one = {{"housing", box(0, 1)}};
    CHECK(decideShell(lone, one, 0.3)[0] == 0);
}

TEST_CASE("names: shells and the mechanism") {
    CHECK(isShellName("Motor_Housing"));
    CHECK(isShellName("end cap"));
    CHECK(isShellName("bearing_cap_2"));
    CHECK_FALSE(isShellName("capacitor"));
    CHECK(isMechanismName("Planet Gear 3"));
    CHECK(shellViewDirections().size() == 14);
}

TEST_CASE("parts whose boxes touch are one assembly, and a gap keeps assemblies apart") {
    const auto at = [](double x, double size) {
        return Aabb{simd_make_double3(x, 0, 0), simd_make_double3(x + size, size, size)};
    };
    // Two touching boxes, a third joined to them through the second, a box 5 apart, and a box
    // apart on its own.
    const std::vector<Aabb> boxes = {at(0, 10), at(10, 10), at(19, 2), at(26, 10), at(100, 1)};
    const std::vector<u32> group = clusterParts(boxes);
    CHECK(group[0] == group[1]);
    CHECK(group[1] == group[2]);
    CHECK(group[3] != group[0]);
    CHECK(group[4] != group[0]);
    CHECK(group[4] != group[3]);
}

TEST_CASE("a plate spanning many parts joins them, however small they are") {
    std::vector<Aabb> boxes;
    for (int i = 0; i < 50; ++i) {
        boxes.push_back({simd_make_double3(i * 10.0, 0, 1), simd_make_double3(i * 10.0 + 1, 1, 2)});
    }
    boxes.push_back({simd_make_double3(0, 0, 0), simd_make_double3(500, 1, 1)}); // under them all
    const std::vector<u32> group = clusterParts(boxes);
    for (const u32 g : group) {
        CHECK(g == group[0]);
    }
}

TEST_CASE("copies of one shape are decided alike") {
    // Two copies of a ring around a hidden part each: the first hides its part well, the second,
    // seen where its part shows less, too little to qualify alone. One shape, one decision: both
    // fade. A pin beside them hides nothing and stays solid.
    ShellCounts counts(5);
    std::vector<u32> front(220, 0), behind(220, 0);
    for (int i = 0; i < 100; ++i) {
        front[i] = 1;
        behind[i] = i < 40 ? 2 : 0;
    }
    for (int i = 100; i < 200; ++i) {
        front[i] = 3;
        behind[i] = i < 108 ? 4 : 0;
    }
    for (int i = 200; i < 220; ++i) {
        front[i] = 5;
    }
    counts.accumulate(front, behind);
    std::vector<ShellPart> parts = {{"ring", box(0, 10), 7},
                                    {"part_a", box(2, 8)},
                                    {"ring", box(20, 30), 7},
                                    {"part_b", box(22, 28)},
                                    {"pin", box(40, 41)}};
    const std::vector<u8> alike = decideShell(counts, parts, 0.3);
    CHECK(alike[0] == 1);
    CHECK(alike[2] == 1);
    // Told apart, the second is under the threshold.
    parts[2].definition = 8;
    CHECK(decideShell(counts, parts, 0.3)[2] == 0);
}
