// Renders known geometry on the real GPU and checks the results, including the pick path that hover
// and adaptive rotation depend on.
#include "cache/blob_writer.h"
#include "gpu/renderer.h"
#include "nav/camera.h"
#include "render/view.h"

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <set>
#include <unistd.h>

using namespace geo;

namespace {

constexpr u32 kWidth = 256;
constexpr u32 kHeight = 256;

void addQuad(std::vector<float>& corners, std::array<float, 3> a, std::array<float, 3> b,
             std::array<float, 3> c, std::array<float, 3> d) {
    for (const auto& p : {a, b, c, a, c, d}) {
        corners.insert(corners.end(), p.begin(), p.end());
    }
}

// A closed box from (-1, -1, -1) to (1, 1, 1), wound outward.
import::TriangleSoup box() {
    import::TriangleSoup soup;
    auto& c = soup.corners;
    addQuad(c, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1});
    addQuad(c, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1});
    addQuad(c, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1});
    addQuad(c, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1});
    addQuad(c, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1});
    addQuad(c, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1});
    return soup;
}

struct Stage {
    std::unique_ptr<gpu::Renderer> renderer;
    nav::Viewport viewport{double(kWidth), double(kHeight)};
    nav::Camera camera;

    explicit Stage(const import::TriangleSoup& soup, bool perspective = false) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool hasDevice = device != nil; // doctest cannot print an Objective-C pointer
        REQUIRE(hasDevice);
        renderer = gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);

        char path[] = "/tmp/georenderXXXXXX";
        const int fd = mkstemp(path);
        REQUIRE(fd >= 0);
        std::string error;
        REQUIRE(import::writeBlob(fd, import::packMesh(import::buildIndexedMesh(soup)), {}, error));
        scene::MappedBlob blob;
        REQUIRE_MESSAGE(blob.open(fd, error), error);
        close(fd);
        unlink(path);
        renderer->setScene(blob);
        renderer->resize(kWidth, kHeight);

        camera.perspective = perspective;
        camera.fovY = 40.0 * M_PI / 180.0;
        const Aabb bounds = renderer->sceneBounds();
        camera.lookAt(simd_make_double3(3, 2, 4), bounds.center(), simd_make_double3(0, 1, 0));
        nav::fit(camera, bounds, viewport, 1.2);
    }

    render::ViewState view(u32 hovered = 0, u32 selected = 0) const {
        render::ViewState state = render::makeView(camera, viewport, renderer->sceneBounds());
        state.uniforms.hoveredId = hovered;
        state.uniforms.selectedId = selected;
        return state;
    }

    std::array<u8, 3> pixel(const std::vector<u8>& bgra, int x, int y) const {
        const usize offset = (usize(y) * kWidth + usize(x)) * 4;
        return {bgra[offset + 2], bgra[offset + 1], bgra[offset]};
    }
};

Vec3 worldHit(const Stage& stage, const render::ViewState& view, int x, int y) {
    const std::optional<gpu::PixelHit> hit = stage.renderer->readPixel(x, y, view.depth);
    REQUIRE(hit.has_value());
    const double ndcX = 2.0 * (double(x) + 0.5) / kWidth - 1.0;
    const double ndcY = 1.0 - 2.0 * (double(y) + 0.5) / kHeight;
    return stage.camera.unproject(ndcX, ndcY, hit->viewDepth, stage.viewport);
}

bool onBoxSurface(Vec3 p, double tolerance) {
    const double largest = std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    return std::abs(largest - 1.0) < tolerance;
}

} // namespace

TEST_CASE("the background is paper and the model is drawn over it") {
    @autoreleasepool {
        Stage stage(box());
        const std::vector<u8> image = stage.renderer->renderOffscreen(stage.view());
        REQUIRE(image.size() == usize(kWidth) * kHeight * 4);
        CHECK(stage.pixel(image, 2, 2) == std::array<u8, 3>{0xF2, 0xEF, 0xE7});
        CHECK(stage.pixel(image, kWidth / 2, kHeight / 2) != std::array<u8, 3>{0xF2, 0xEF, 0xE7});
    }
}

TEST_CASE("a pixel on the model reports a pick, and empty space does not") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        CHECK(stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth).has_value());
        CHECK_FALSE(stage.renderer->readPixel(2, 2, view.depth).has_value());
    }
}

TEST_CASE("positions outside the viewport are rejected, not read") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        CHECK_FALSE(stage.renderer->readPixel(-1, 0, view.depth).has_value());
        CHECK_FALSE(stage.renderer->readPixel(0, -1, view.depth).has_value());
        CHECK_FALSE(stage.renderer->readPixel(kWidth, 0, view.depth).has_value());
        CHECK_FALSE(stage.renderer->readPixel(0, kHeight, view.depth).has_value());
    }
}

TEST_CASE("the three visible faces of a box have three different ids") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        std::set<u32> ids;
        for (u32 y = 0; y < kHeight; y += 2) {
            for (u32 x = 0; x < kWidth; x += 2) {
                if (const auto hit = stage.renderer->readPixel(int(x), int(y), view.depth)) {
                    ids.insert(hit->id);
                }
            }
        }
        CHECK(ids.size() == 3);
    }
}

TEST_CASE("unprojected picks land on the surface") {
    for (const bool perspective : {false, true}) {
        @autoreleasepool {
            Stage stage(box(), perspective);
            const render::ViewState view = stage.view();
            stage.renderer->renderOffscreen(view);
            int checked = 0;
            for (u32 y = 8; y < kHeight; y += 8) {
                for (u32 x = 8; x < kWidth; x += 8) {
                    if (stage.renderer->readPixel(int(x), int(y), view.depth)) {
                        CHECK(onBoxSurface(worldHit(stage, view, int(x), int(y)), 0.02));
                        ++checked;
                    }
                }
            }
            CHECK(checked > 100);
        }
    }
}

TEST_CASE("hover changes only the hovered face, and selection shows through it") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState plain = stage.view();
        const std::vector<u8> before = stage.renderer->renderOffscreen(plain);
        const u32 id = stage.renderer->readPixel(kWidth / 2, kHeight / 2, plain.depth)->id;

        const std::vector<u8> hovered = stage.renderer->renderOffscreen(stage.view(id));
        usize changed = 0, changedElsewhere = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const bool differs =
                    stage.pixel(before, int(x), int(y)) != stage.pixel(hovered, int(x), int(y));
                const auto hit = stage.renderer->readPixel(int(x), int(y), plain.depth);
                changed += differs;
                // Antialiased border pixels mix in the neighbouring face, so only interiors count.
                changedElsewhere +=
                    differs && (!hit || hit->id != id) && x % 16 == 8 && y % 16 == 8;
            }
        }
        CHECK(changed > 1000);
        CHECK(changedElsewhere == 0);

        const std::vector<u8> selected = stage.renderer->renderOffscreen(stage.view(0, id));
        CHECK(stage.pixel(selected, kWidth / 2, kHeight / 2) !=
              stage.pixel(before, kWidth / 2, kHeight / 2));
    }
}

TEST_CASE("an open surface shows both sides") {
    @autoreleasepool {
        import::TriangleSoup soup;
        addQuad(soup.corners, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}); // faces +z
        Stage stage(soup);
        stage.camera.lookAt(simd_make_double3(0, 0, -5), simd_make_double3(0, 0, 0),
                            simd_make_double3(0, 1, 0)); // looks at its back
        nav::fit(stage.camera, stage.renderer->sceneBounds(), stage.viewport, 1.2);
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        CHECK(stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth).has_value());
    }
}

TEST_CASE("a closed surface culls its back faces, so a pick finds the near side") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        const Vec3 hit = worldHit(stage, view, kWidth / 2, kHeight / 2);
        CHECK(simd_dot(hit, stage.camera.back()) > 0.0);
    }
}

TEST_CASE("a wide definition decodes through its high bits") {
    @autoreleasepool {
        // One tiny detail far from a large triangle forces the 24-bit grid.
        import::TriangleSoup soup = box();
        const std::array<float, 9> far = {100000, 0, 0, 100001, 0, 0, 100000, 1, 0};
        soup.corners.insert(soup.corners.end(), far.begin(), far.end());
        Stage stage(soup);
        const Aabb detail{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
        nav::fit(stage.camera, detail, stage.viewport, 1.2);
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        REQUIRE(stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth).has_value());
    }
}
