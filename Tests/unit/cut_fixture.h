// Boxes as the parts of one assembly, for checking cuts through parts that touch, reach into each
// other or enclose a void.
#pragma once

#include "step/step_import.h"
#include "step_fixture.h"

#include <array>
#include <doctest/doctest.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace geo::test {

struct CutBox {
    std::array<double, 3> lo, hi;
    bool cavity = false;
};

/// Imports the boxes, in order, into a cache file. Returns its path.
inline std::string importBoxes(const std::vector<CutBox>& boxes, const char* workerPath) {
    StepWriter writer;
    const int root = writer.assembly("scene");
    for (usize i = 0; i < boxes.size(); ++i) {
        const int shape = writer.box(boxes[i].lo, boxes[i].hi, boxes[i].cavity);
        writer.place(root, writer.part("box" + std::to_string(i), shape), shape, {0, 0, 0});
    }
    const std::string text = writer.text();
    char path[] = "/tmp/geocutXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::StepOptions options;
    options.workerPath = workerPath;
    import::StepStats stats;
    std::string error;
    const bool ok = import::importStep({reinterpret_cast<const u8*>(text.data()), text.size()}, fd,
                                       nullptr, options, stats, error);
    close(fd);
    REQUIRE_MESSAGE(ok, error);
    return path;
}

/// A big box, and a small one wholly below a cut at z = 2 that reaches a unit into it, so rays
/// through the big one's cut pass in and out of the small one before leaving the big one.
inline std::vector<CutBox> reachingBoxes() {
    return {{{0, 0, 0}, {4, 4, 4}}, {{3, 1, 1}, {6, 3, 1.5}}};
}

} // namespace geo::test
