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
