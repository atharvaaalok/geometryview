// Edges hidden behind a face stay hidden, in Metal: see hidden_box_fixture.h. And the edges a hidden
// or ghosted face leaves are inked. Needs the OCCT worker, since the assemblies are STEP.
// webgpu_test.mm checks the WebGPU renderer the same way.

#if defined(GEO_TEST_WORKER_PATH)

#include "core/look_state.h"
#include "core/offscreen_view.h"
#include "cut_fixture.h"
#include "engine.h"
#include "gpu/renderer.h"
#include "hidden_box_fixture.h"
#include "scene/scene.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <doctest/doctest.h>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <vector>

using namespace geo;
namespace fs = std::filesystem;

namespace {

// Luminance of each pixel of a PNG, or nothing if it cannot be read.
std::vector<u8> readGray(const std::string& path, u32& width, u32& height) {
    NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (source == nullptr) {
        return {};
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (image == nullptr) {
        return {};
    }
    width = u32(CGImageGetWidth(image));
    height = u32(CGImageGetHeight(image));
    std::vector<u8> gray(usize(width) * height);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(gray.data(), width, height, 8, width, space,
                                                 u32(kCGImageAlphaNone));
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return gray;
}

} // namespace

TEST_CASE("a part's edges behind another's face stay hidden, in an assembly with a coarse tolerance") {
    @autoreleasepool {
        const std::string blob = test::importHiddenBox(GEO_TEST_WORKER_PATH);
        const std::string png = (fs::temp_directory_path() / "edge-leak.png").string();
        const GeoRenderOptions options = test::hiddenBoxView();
        REQUIRE(geo_render_blob_to_png(GEO_TEST_SHADER_LIBRARY, blob.c_str(), png.c_str(), &options));
        u32 width = 0, height = 0;
        const std::vector<u8> gray = readGray(png, width, height);
        REQUIRE(!gray.empty());
        test::checkNothingShowsThrough(gray, width, height);
        fs::remove(png);
        unlink(blob.c_str());
    }
}

TEST_CASE("a thin wall hides what is just behind it, in an assembly with a coarse tolerance") {
    @autoreleasepool {
        const std::string blob = test::importThinTube(GEO_TEST_WORKER_PATH);
        const std::string png = (fs::temp_directory_path() / "edge-leak-tube.png").string();
        const GeoRenderOptions options = test::thinTubeView();
        REQUIRE(
            geo_render_blob_to_png(GEO_TEST_SHADER_LIBRARY, blob.c_str(), png.c_str(), &options));
        u32 width = 0, height = 0;
        const std::vector<u8> gray = readGray(png, width, height);
        REQUIRE(!gray.empty());
        test::checkWallHidesItsInside(gray, width, height);
        fs::remove(png);
        unlink(blob.c_str());
    }
}

TEST_CASE("a smooth edge is inked where it borders a hidden or ghosted face, and nowhere else") {
    @autoreleasepool {
        const std::string blob = test::importBoxes(test::splitTopBox(), GEO_TEST_WORKER_PATH);
        std::unique_ptr<gpu::Renderer> renderer =
            gpu::Renderer::create(MTLCreateSystemDefaultDevice(), GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);
        GeoRenderOptions options = geo_default_render_options();
        options.width = options.height = 400;
        options.overlay = false;
        options.azimuth_degrees = 0;
        options.elevation_degrees = 60;
        options.look.style = GeoStyleTechnical;
        options.look.cast_shadow = false;
        const int fd = open(blob.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<std::shared_ptr<scene::Segment>> segments;
        std::string error;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        unlink(blob.c_str());
        engine::OffscreenView frame;
        REQUIRE_MESSAGE(engine::prepareOffscreen(*renderer, segments, options, frame, error),
                        error);
        model::FaceState state;
        state.resize(frame.scene.faceCounts());
        renderer->syncState(state);
        const std::vector<u8> plain = renderer->renderOffscreen(frame.view);

        // The top's two halves, and where they meet in each row: the seam.
        const auto idAt = [&](int x, int y) -> u32 {
            const auto hit = renderer->readPixel(x, y, frame.view.depth);
            return hit ? hit->id : 0;
        };
        u32 left = 0, right = 0;
        std::vector<std::array<int, 2>> seam;
        for (int y = 0; y < 400; y += 3) {
            for (int x = 1; x < 400; ++x) {
                const u32 a = idAt(x - 1, y), b = idAt(x, y);
                if (a != 0 && b != 0 && a != b) {
                    const auto ta = frame.scene.resolve(a), tb = frame.scene.resolve(b);
                    // Both on the top: of the faces seen from above, the two halves meet only
                    // along the seam, which crosses the middle of the picture.
                    if (std::abs(x - 200) < 40) {
                        left = left == 0 ? ta->face : left;
                        right = right == 0 ? tb->face : right;
                        if (ta->face == left && tb->face == right) {
                            seam.push_back({x, y});
                        }
                    }
                }
            }
        }
        REQUIRE(left != right);
        REQUIRE(seam.size() > 20);
        // The share of the seam marked darker than `below`, by a pixel within two of it. A crease
        // is darker than 40; the composite's outline where the depth jumps at a gap is about 60.
        const auto marked = [&](const std::vector<u8>& bgra, int below) {
            usize count = 0;
            for (const auto& [x, y] : seam) {
                int least = 255;
                for (int dx = -2; dx <= 2; ++dx) {
                    const usize i = (usize(y) * 400 + usize(x + dx)) * 4;
                    least = std::min({least, int(bgra[i]), int(bgra[i + 1]), int(bgra[i + 2])});
                }
                count += least < below ? 1 : 0;
            }
            return double(count) / double(seam.size());
        };
        const auto inked = [&](const std::vector<u8>& bgra) { return marked(bgra, 40); };
        CAPTURE(left);
        CAPTURE(right);
        CHECK(inked(plain) < 0.1); // a smooth edge between two shown faces: no line
        state.set({0, left}, kStateHidden, true);
        renderer->syncState(state);
        CHECK(inked(renderer->renderOffscreen(frame.view)) > 0.9); // the rim of what it opened
        state.set({0, left}, kStateHidden, false);
        state.set({0, left}, kStateGhost, true);
        renderer->syncState(state);
        CHECK(inked(renderer->renderOffscreen(frame.view)) > 0.9); // so for a ghost
        state.set({0, left}, kStateGhost, false);
        state.set({0, left}, kStateHidden, true);
        state.set({0, right}, kStateHidden, true);
        renderer->syncState(state);
        CHECK(inked(renderer->renderOffscreen(frame.view)) < 0.1); // both gone: no line

        // A part ghosted whole, its ghost dashed lines only: the seam is a dash where it borders a
        // hidden face, and nothing where both sides are ghosted.
        const auto dashed = [&](const std::vector<u8>& bgra) { return marked(bgra, 200); };
        engine::LookState looks;
        GeoLook look = options.look;
        look.ghost_style = GeoGhostLines;
        looks.set(engine::lookSettings(look));
        looks.apply(*renderer, frame.scene);
        state = model::FaceState();
        state.resize(frame.scene.faceCounts());
        state.set({0, model::Target::kWholeOccurrence}, kStateGhost, true);
        renderer->syncState(state);
        CHECK(dashed(renderer->renderOffscreen(frame.view)) < 0.05);
        state.set({0, left}, kStateHidden, true);
        renderer->syncState(state);
        CHECK(dashed(renderer->renderOffscreen(frame.view)) > 0.4); // 0.6: the dashes and gaps
    }
}

#endif
