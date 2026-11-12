// A large box with a small one wholly behind it, in an assembly whose far corner makes its import
// tolerance coarse, as a big rack's does; and the check that, seen from the front, nothing of the
// small box shows through the large one's face while the face's outline is drawn. Before edges were
// lifted by each part's own chord error, the small box's edges were lifted by twice the
// assembly's coarsest tolerance (8 mm here), through the 6 mm between it and the front face.
#pragma once

#include "engine.h"
#include "step/step_import.h"
#include "step_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
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

// A tube 40 mm across with a 2 mm wall, in an assembly made coarse by a speck 20 m away, so its
// definition's chord lift is about 8 mm, four times its wall (Tests/data/thin_tube.step, written by
// build123d: a cylinder of radius 20 less one of 18, 30 tall). Seen from a little above, the inside
// circle at its foot is 2 mm behind the wall's front: lifted by the chord error it showed through
// as a second ellipse, as the far side of a fan blade at its edges showed through the blade. A
// segment between two faces is lifted only by the floor, and stays hidden.
inline std::string importThinTube(const char* workerPath) {
    std::ifstream file(GEO_TEST_DATA_DIR "/thin_tube.step", std::ios::binary);
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    char path[] = "/tmp/geotubeXXXXXX";
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

inline GeoRenderOptions thinTubeView() {
    GeoRenderOptions options = hiddenBoxView();
    options.elevation_degrees = 25.0;
    options.isolate = "tube";
    return options;
}

// Below the tube's front, across its middle, the foot's outline is the only ink: nothing a few
// pixels above it, where the inside circle would show through the wall.
inline void checkWallHidesItsInside(const std::vector<u8>& gray, u32 width, u32 height) {
    u32 left = width, right = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (gray[usize(y) * width + x] < 96) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    REQUIRE(right > left + 200);
    u32 columns = 0, leaked = 0;
    for (u32 x = left + (right - left) * 3 / 10; x < right - (right - left) * 3 / 10; ++x) {
        u32 foot = 0;
        for (u32 y = height; y-- > 0;) {
            if (gray[usize(y) * width + x] < 96) {
                foot = y;
                break;
            }
        }
        REQUIRE(foot > 40);
        ++columns;
        for (u32 y = foot - 40; y < foot - 6; ++y) {
            leaked += gray[usize(y) * width + x] < 160 ? 1 : 0;
        }
    }
    CHECK(columns > 100);
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
