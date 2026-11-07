// Renders known geometry on the real GPU and checks the results, including the pick path that hover
// and adaptive rotation depend on.
#include "gpu/renderer.h"
#include "import.h"
#include "nav/camera.h"
#include "render/view.h"
#include "scene/scene.h"

#include <array>
#include <cmath>
#include <cstring>
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

// Writes triangles as a binary STL, so tests import through the real path.
std::string binaryStl(const std::vector<float>& corners) {
    const u32 triangles = u32(corners.size() / 9);
    std::string file(84 + usize(triangles) * 50, '\0');
    std::memcpy(file.data() + 80, &triangles, sizeof(triangles));
    for (u32 t = 0; t < triangles; ++t) {
        std::memcpy(file.data() + 84 + usize(t) * 50 + 12, corners.data() + usize(t) * 9, 36);
    }
    return file;
}

// A closed box from (-1, -1, -1) to (1, 1, 1), wound outward.
std::vector<float> box() {
    std::vector<float> c;
    addQuad(c, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1});
    addQuad(c, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1});
    addQuad(c, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1});
    addQuad(c, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1});
    addQuad(c, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1});
    addQuad(c, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1});
    return c;
}

struct Stage {
    std::unique_ptr<gpu::Renderer> renderer;
    nav::Viewport viewport{double(kWidth), double(kHeight)};
    nav::Camera camera;
    scene::Scene scene;
    model::FaceState state;
    Aabb bounds{};

    explicit Stage(const std::vector<float>& corners, bool perspective = false) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool hasDevice = device != nil; // doctest cannot print an Objective-C pointer
        REQUIRE(hasDevice);
        renderer = gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);

        char path[] = "/tmp/georenderXXXXXX";
        const int fd = mkstemp(path);
        REQUIRE(fd >= 0);
        std::string error;
        const std::string stl = binaryStl(corners);
        import::ImportResult result;
        REQUIRE(import::importMesh({reinterpret_cast<const u8*>(stl.data()), stl.size()}, fd,
                                   nullptr, result, error));
        std::vector<std::shared_ptr<scene::MappedSegment>> segments;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        unlink(path);
        scene::Document& document = scene.document(scene.addDocument());
        for (std::shared_ptr<scene::MappedSegment>& segment : segments) {
            REQUIRE_MESSAGE(document.add(std::move(segment), error), error);
        }
        renderer->syncScene(scene);
        state.resize(scene.faceCounts());
        renderer->syncState(state);
        renderer->resize(kWidth, kHeight);
        bounds = scene.bounds().value();

        camera.perspective = perspective;
        camera.fovY = 40.0 * M_PI / 180.0;
        camera.lookAt(simd_make_double3(3, 2, 4), bounds.center(), simd_make_double3(0, 1, 0));
        nav::fit(camera, bounds, viewport, 1.2);
    }

    // Selection lives in face state now, so selecting marks the face and uploads the change.
    render::ViewState view(u32 hovered = 0, u32 selected = 0) {
        if (selected != 0) {
            const std::optional<scene::PickTarget> target = scene.resolve(selected);
            REQUIRE(target.has_value());
            state.set({target->globalOccurrence, target->face}, kStateSelected, true);
        }
        renderer->syncState(state);
        render::ViewState view = render::makeView(camera, viewport, bounds);
        if (hovered != 0) {
            view.uniforms.hoverFirst = hovered;
            view.uniforms.hoverEnd = hovered + 1;
        }
        return view;
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
        std::vector<float> corners;
        addQuad(corners, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}); // faces +z
        Stage stage(corners);
        stage.camera.lookAt(simd_make_double3(0, 0, -5), simd_make_double3(0, 0, 0),
                            simd_make_double3(0, 1, 0)); // looks at its back
        nav::fit(stage.camera, stage.bounds, stage.viewport, 1.2);
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
        std::vector<float> corners = box();
        const std::array<float, 9> far = {100000, 0, 0, 100001, 0, 0, 100000, 1, 0};
        corners.insert(corners.end(), far.begin(), far.end());
        Stage stage(corners);
        const Aabb detail{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
        nav::fit(stage.camera, detail, stage.viewport, 1.2);
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        REQUIRE(stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth).has_value());
    }
}

namespace {

std::set<u32> visibleIds(Stage& stage, const render::ViewState& view) {
    std::set<u32> ids;
    for (u32 y = 0; y < kHeight; y += 2) {
        for (u32 x = 0; x < kWidth; x += 2) {
            if (const auto hit = stage.renderer->readPixel(int(x), int(y), view.depth)) {
                ids.insert(hit->id);
            }
        }
    }
    return ids;
}

} // namespace

TEST_CASE("a hidden face is neither drawn nor picked") {
    @autoreleasepool {
        Stage stage(box());
        render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        const std::set<u32> before = visibleIds(stage, view);
        REQUIRE(before.size() == 3);
        const auto target = stage.scene.resolve(*before.begin());
        stage.state.set({target->globalOccurrence, target->face}, kStateHidden, true);
        view = stage.view();
        stage.renderer->renderOffscreen(view);
        const std::set<u32> after = visibleIds(stage, view);
        // The hidden face's place is taken by the inside of the box, seen through the gap.
        CHECK_FALSE(after.contains(*before.begin()));
    }
}

TEST_CASE("a ghost is drawn translucent and clicks pass through it unless the tool is held") {
    @autoreleasepool {
        Stage stage(box());
        render::ViewState view = stage.view();
        const std::vector<u8> plain = stage.renderer->renderOffscreen(view);
        const std::set<u32> before = visibleIds(stage, view);
        const u32 front = *before.begin();
        const auto target = stage.scene.resolve(front);
        stage.state.set({target->globalOccurrence, target->face}, kStateGhost, true);

        view = stage.view();
        const std::vector<u8> ghosted = stage.renderer->renderOffscreen(view);
        CHECK(ghosted != plain);
        CHECK_FALSE(visibleIds(stage, view).contains(front));

        view = stage.view();
        view.uniforms.ghostPickable = 1;
        stage.renderer->renderOffscreen(view);
        CHECK(visibleIds(stage, view).contains(front));
    }
}

TEST_CASE("hiding a whole occurrence leaves an empty picture") {
    @autoreleasepool {
        Stage stage(box());
        stage.state.set({0, model::Target::kWholeOccurrence}, kStateHidden, true);
        const render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        CHECK(visibleIds(stage, view).empty());
    }
}
