// A grooved block's creases, seen from all round: for checking that a crease is inked wherever it
// can be seen, on either renderer.
//
// Tests/data/groove_block.step, written by build123d: a block 120 along X, 60 across and 30 tall,
// its top at z = 0, with two grooves along X centred at y = ±13, each 1.5 deep with walls at 45
// degrees, 20 wide at the top and 17 at the floor, as on a linear bushing's housing. Each wall
// meets the land above in a convex crease and the floor below in a concave one.
#pragma once

#include "core/offscreen_view.h"
#include "engine.h"
#include "nav/camera.h"
#include "render/backend.h"
#include "render/view.h"
#include "scene/scene.h"
#include "step/step_import.h"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace geo::test {

inline std::string importGrooveBlock(const char* workerPath) {
    std::ifstream file(GEO_TEST_DATA_DIR "/groove_block.step", std::ios::binary);
    REQUIRE(file);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    char path[] = "/tmp/geogrooveXXXXXX";
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

struct CreaseSweep {
    int views = 0;
    int short_ = 0;     // creases inked along less than 98% of what shows of them
    double worst = 1.0; // the least inked, of those
    std::string worstView;
};

/// Draws the block from all round, in Illustration: in ortho framed whole, and in perspective
/// close to the grooves, as a person zoomed in would be. For each crease, samples its middle
/// along its length; where nothing nearer covers it, the line there must be dark. What covers it
/// is read from `picker`'s picks where given, drawing the same views: WebGPU's come back frames
/// late.
inline CreaseSweep sweepCreases(render::Backend& backend, const std::string& blob,
                                render::Backend* picker = nullptr) {
    struct Crease {
        double y, z;
        const char* name;
    };
    constexpr Crease kCreases[] = {
        {-23.0, 0.0, "land into the near groove"},
        {-21.5, -1.5, "the near groove's floor, its near side"},
        {-4.5, -1.5, "the near groove's floor, its far side"},
        {-3.0, 0.0, "the middle land, near"},
        {3.0, 0.0, "the middle land, far"},
        {4.5, -1.5, "the far groove's floor, its near side"},
        {21.5, -1.5, "the far groove's floor, its far side"},
        {23.0, 0.0, "land out of the far groove"},
    };
    constexpr u32 kW = 800, kH = 600;
    GeoRenderOptions options = geo_default_render_options();
    options.width = kW;
    options.height = kH;
    options.overlay = false;
    options.look.style = GeoStyleIllustration;
    options.look.cast_shadow = false;
    options.look.grain = 0.0f;
    options.look.wobble = 0.0f;
    std::string error;
    const auto prepare = [&](render::Backend& b, engine::OffscreenView& frame) {
        const int fd = open(blob.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<std::shared_ptr<scene::Segment>> segments;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        REQUIRE_MESSAGE(engine::prepareOffscreen(b, segments, options, frame, error), error);
        model::FaceState state;
        state.resize(frame.scene.faceCounts());
        b.syncState(state);
    };
    engine::OffscreenView frame, picked;
    prepare(backend, frame);
    if (picker != nullptr) {
        prepare(*picker, picked);
    }
    render::Backend& picks = picker != nullptr ? *picker : backend;
    const Aabb bounds = frame.scene.bounds().value();
    const nav::Viewport viewport{double(kW), double(kH)};

    CreaseSweep sweep;
    for (const bool perspective : {false, true}) {
        for (int azimuth = 0; azimuth < 360; azimuth += 30) {
            for (int elevation = 15; elevation <= 88; elevation += perspective ? 8 : 6) {
                for (const double distance :
                     perspective ? std::vector<double>{45.0, 110.0} : std::vector<double>{0.0}) {
                    const double a = azimuth * M_PI / 180.0, e = elevation * M_PI / 180.0;
                    const Vec3 direction = simd_make_double3(
                        std::cos(e) * std::sin(a), -std::cos(e) * std::cos(a), std::sin(e));
                    nav::Camera camera;
                    camera.perspective = perspective;
                    const Vec3 target =
                        perspective ? simd_make_double3(-30.0, 0.0, -1.0) : bounds.center();
                    camera.lookAt(target + direction * (perspective ? distance : 1.0), target,
                                  simd_make_double3(0, 0, 1));
                    if (!perspective) {
                        nav::fit(camera, bounds, viewport, 1.05);
                    }
                    frame.view = render::makeView(camera, viewport, bounds);
                    const std::vector<u8> bgra = backend.renderOffscreen(frame.view, nullptr);
                    if (picker != nullptr) {
                        picker->renderOffscreen(frame.view, nullptr);
                    }
                    ++sweep.views;
                    for (const Crease& crease : kCreases) {
                        int visible = 0, inked = 0;
                        for (int i = 0; i <= 240; ++i) {
                            const Vec3 p =
                                simd_make_double3(-58.0 + 116.0 * i / 240.0, crease.y, crease.z);
                            const nav::NdcPoint ndc = camera.project(p, viewport);
                            const int x = int((ndc.x + 1.0) * 0.5 * kW);
                            const int y = int((1.0 - ndc.y) * 0.5 * kH);
                            if (ndc.viewDepth <= 0.0 || x < 2 || y < 2 || x >= int(kW) - 2 ||
                                y >= int(kH) - 2) {
                                continue;
                            }
                            // Seen where nothing nearer covers the crease's own spot.
                            const auto hit = picks.readPixel(x, y, frame.view.depth);
                            const double pixel = 2.0 * camera.halfHeightAt(ndc.viewDepth) / kH;
                            if (!hit || hit->viewDepth < ndc.viewDepth - 3.0 * pixel) {
                                continue;
                            }
                            ++visible;
                            int least = 255;
                            for (int dy = -1; dy <= 1; ++dy) {
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const usize at = (usize(y + dy) * kW + usize(x + dx)) * 4;
                                    least = std::min(
                                        least, (int(bgra[at]) + bgra[at + 1] + bgra[at + 2]) / 3);
                                }
                            }
                            inked += least < 90 ? 1 : 0;
                        }
                        const double share = visible == 0 ? 1.0 : double(inked) / visible;
                        if (visible >= 24 && share < 0.98) {
                            ++sweep.short_;
                            const std::string view =
                                std::format("{} az {} el {}{}: {} {:.0f}% of {}",
                                            perspective ? "persp" : "ortho", azimuth, elevation,
                                            perspective ? std::format(" at {:.0f}", distance) : "",
                                            crease.name, 100.0 * share, visible);
                            MESSAGE(view);
                            if (share < sweep.worst) {
                                sweep.worst = share;
                                sweep.worstView = view;
                            }
                        }
                    }
                }
            }
        }
    }
    return sweep;
}

} // namespace geo::test
