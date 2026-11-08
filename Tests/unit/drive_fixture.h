// A small assembly for image tests: a drive's worth of named boxes, imported into a cache file.
#pragma once

#include "step/step_import.h"
#include "step_fixture.h"

#include <doctest/doctest.h>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace geo::test {

// A drive's worth of named boxes: names that suggest materials, and two that tell nothing.
inline std::string driveStep() {
    test::StepWriter writer;
    const int root = writer.assembly("drive");
    const auto add = [&](const std::string& name, std::array<double, 3> size,
                         std::array<double, 3> at) {
        const int shape = writer.box({0, 0, 0}, size);
        writer.place(root, writer.part(name, shape), shape, at);
    };
    add("housing", {40, 30, 6}, {0, 0, 0});
    add("Stator_Core", {10, 10, 14}, {3, 3, 6});
    add("winding", {6, 6, 20}, {16, 4, 6});
    add("Magnet 1", {8, 8, 8}, {26, 18, 6});
    add("widget", {12, 6, 10}, {4, 20, 6});
    add("gizmo", {6, 12, 4}, {30, 2, 6});
    return writer.text();
}

inline std::string importDrive(const char* workerPath) {
    const std::string text = driveStep();
    char path[] = "/tmp/geogoldenXXXXXX";
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

} // namespace geo::test
