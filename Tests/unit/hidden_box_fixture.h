// A large box with a small one wholly behind it, in an assembly whose far corner makes its import
// tolerance coarse, as a big rack's does; and the check that, seen from the front, nothing of the
// small box shows through the large one's face while the face's outline is drawn. Before edges were
// lifted by each part's own chord error, the small box's edges were lifted by twice the
// assembly's coarsest tolerance (8 mm here), through the 6 mm between it and the front face.
#pragma once

#include "engine.h"
#include "step/step_import.h"
#include "step_fixture.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace geo::test {

inline std::string hiddenBoxStep() {
    StepWriter writer;
    const int root = writer.assembly("scene");
    const auto add = [&](const std::string& name, std::array<double, 3> lo, std::array<double, 3> hi) {
        const int shape = writer.box(lo, hi);
        writer.place(root, writer.part(name, shape), shape, {0, 0, 0});
    };
    add("front", {0, 0, 0}, {10, 4, 10});
    add("behind", {4, 6, 4}, {6, 8, 6});
    add("speck", {20000, 0, 0}, {20001, 1, 1});
    return writer.text();
}

inline std::string importHiddenBox(const char* workerPath) {
    const std::string text = hiddenBoxStep();
    char path[] = "/tmp/geoleakXXXXXX";
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

// Straight from the front, orthographic, in Technical: white faces and black lines, with no shadow
// or grain to tell apart from ink. The front face fills the frame; the small box projects inside.
inline GeoRenderOptions hiddenBoxView() {
    GeoRenderOptions options = geo_default_render_options();
    options.width = 600;
    options.height = 600;
    options.azimuth_degrees = 0.0;
    options.elevation_degrees = 0.0;
    options.perspective = false;
    options.overlay = false;
    options.look.style = GeoStyleTechnical;
    options.look.cast_shadow = false;
    options.isolate = "front,behind";
    return options;
}

// Checks one image's luminance: the face's outline inked along its sides, nothing inside it.
inline void checkNothingShowsThrough(const std::vector<u8>& gray, u32 width, u32 height) {
    u32 left = width, right = 0, top = height, bottom = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (gray[usize(y) * width + x] < 96) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    REQUIRE(right > left + 100);
    REQUIRE(bottom > top + 100);
    u32 inkedLeft = 0, inkedTop = 0;
    for (u32 y = top + 10; y + 10 < bottom; ++y) {
        inkedLeft += gray[usize(y) * width + left + 1] < 96 ? 1 : 0;
    }
    for (u32 x = left + 10; x + 10 < right; ++x) {
        inkedTop += gray[usize(top + 1) * width + x] < 96 ? 1 : 0;
    }
    CHECK(inkedLeft > (bottom - top - 20) * 9 / 10);
    CHECK(inkedTop > (right - left - 20) * 9 / 10);
    const u32 margin = (right - left) / 10;
    u32 leaked = 0;
    for (u32 y = top + margin; y < bottom - margin; ++y) {
        for (u32 x = left + margin; x < right - margin; ++x) {
            leaked += gray[usize(y) * width + x] < 160 ? 1 : 0;
        }
    }
    CHECK(leaked == 0);
}

// Luminance of BGRA pixels.
inline std::vector<u8> grayOf(const std::vector<u8>& bgra) {
    std::vector<u8> gray(bgra.size() / 4);
    for (usize p = 0; p < gray.size(); ++p) {
        gray[p] = u8((bgra[p * 4] * 29 + bgra[p * 4 + 1] * 150 + bgra[p * 4 + 2] * 77) >> 8);
    }
    return gray;
}

} // namespace geo::test
