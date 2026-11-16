// Renders known geometry on the real GPU and checks the results, including the pick path that hover
// and adaptive rotation depend on.
#include "gpu/renderer.h"
#include "import.h"
#include "nav/camera.h"
#include "render/overlay.h"
#include "render/style.h"
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

    explicit Stage(const std::vector<float>& corners, bool perspective = false)
        : Stage(std::vector<std::vector<float>>{corners}, perspective) {}

    // Several parts, each imported as a document of its own: a closed one and an open one, say.
    explicit Stage(const std::vector<std::vector<float>>& parts, bool perspective = false) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool hasDevice = device != nil; // doctest cannot print an Objective-C pointer
        REQUIRE(hasDevice);
        renderer = gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);

        for (const std::vector<float>& corners : parts) {
            char path[] = "/tmp/georenderXXXXXX";
            const int fd = mkstemp(path);
            REQUIRE(fd >= 0);
            std::string error;
            const std::string stl = binaryStl(corners);
            import::ImportResult result;
            REQUIRE(import::importMesh({reinterpret_cast<const u8*>(stl.data()), stl.size()}, fd,
                                       nullptr, result, error));
            std::vector<std::shared_ptr<scene::Segment>> segments;
            REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
            close(fd);
            unlink(path);
            scene::Document& document = scene.document(scene.addDocument());
            for (std::shared_ptr<scene::Segment>& segment : segments) {
                REQUIRE_MESSAGE(document.add(std::move(segment), error), error);
            }
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
        // Paper grain moves the sheet a few levels toward the ink at most.
        const std::array<u8, 3> paper = {0xF2, 0xEF, 0xE7};
        const auto nearPaper = [&](const std::array<u8, 3>& c) {
            for (usize i = 0; i < 3; ++i) {
                if (c[i] > paper[i] || paper[i] - c[i] > 24) {
                    return false;
                }
            }
            return true;
        };
        CHECK(nearPaper(stage.pixel(image, 2, 2)));
        CHECK_FALSE(nearPaper(stage.pixel(image, kWidth / 2, kHeight / 2)));
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

TEST_CASE("hover changes only the hovered face and its outline, and selection shows through") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState plain = stage.view();
        const std::vector<u8> before = stage.renderer->renderOffscreen(plain);
        const u32 id = stage.renderer->readPixel(kWidth / 2, kHeight / 2, plain.depth)->id;

        const std::vector<u8> hovered = stage.renderer->renderOffscreen(stage.view(id));
        // The hovered face's highlight reaches a few pixels past its edges.
        const auto nearFace = [&](int x, int y) {
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const auto hit = stage.renderer->readPixel(x + dx, y + dy, plain.depth);
                    if (hit && hit->id == id) {
                        return true;
                    }
                }
            }
            return false;
        };
        usize changed = 0, changedElsewhere = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const bool differs =
                    stage.pixel(before, int(x), int(y)) != stage.pixel(hovered, int(x), int(y));
                changed += differs;
                changedElsewhere +=
                    differs && x % 16 == 8 && y % 16 == 8 && !nearFace(int(x), int(y));
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

TEST_CASE("closed and open surfaces in one scene both draw") {
    @autoreleasepool {
        // A closed box, and beside it an open sheet: the scene's batches are the box's, then the
        // sheet's, and each list must draw its own.
        std::vector<float> sheet;
        addQuad(sheet, {2, -1, 0}, {4, -1, 0}, {4, 1, 0}, {2, 1, 0}); // faces +z
        Stage stage(std::vector<std::vector<float>>{box(), sheet});
        for (const double z : {5.0, -5.0}) { // its front, then its back
            CAPTURE(z);
            stage.camera.lookAt(simd_make_double3(1, 0, z), simd_make_double3(1, 0, 0),
                                simd_make_double3(0, 1, 0));
            nav::fit(stage.camera, stage.bounds, stage.viewport, 1.2);
            const render::ViewState view = stage.view();
            stage.renderer->renderOffscreen(view);
            std::set<u32> occurrences;
            for (const u32 id : visibleIds(stage, view)) {
                if (const auto target = stage.scene.resolve(id)) {
                    occurrences.insert(target->globalOccurrence);
                }
            }
            CHECK(occurrences.contains(0)); // the box
            CHECK(occurrences.contains(1)); // the sheet
        }
    }
}

TEST_CASE("a hidden face is neither drawn nor picked, and the inside shows through the gap") {
    for (const bool perspective : {false, true}) {
        @autoreleasepool {
            CAPTURE(perspective);
            Stage stage(box(), perspective);
            render::ViewState view = stage.view();
            stage.renderer->renderOffscreen(view);
            REQUIRE(visibleIds(stage, view).size() == 3);
            // The face in the middle of the picture, and every pixel it covers.
            const auto middle = stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth);
            REQUIRE(middle.has_value());
            const u32 hidden = middle->id;
            struct Covered {
                int x, y;
                Vec3 at; // where the face was
            };
            std::vector<Covered> covered;
            for (u32 y = 0; y < kHeight; y += 2) {
                for (u32 x = 0; x < kWidth; x += 2) {
                    const auto hit = stage.renderer->readPixel(int(x), int(y), view.depth);
                    if (hit && hit->id == hidden) {
                        covered.push_back({int(x), int(y), worldHit(stage, view, int(x), int(y))});
                    }
                }
            }
            REQUIRE(covered.size() > 200);
            const auto target = stage.scene.resolve(hidden);
            stage.state.set({target->globalOccurrence, target->face}, kStateHidden, true);
            view = stage.view();
            stage.renderer->renderOffscreen(view);
            CHECK_FALSE(visibleIds(stage, view).contains(hidden));
            // Where it was, the box's inside: its walls, seen from within, behind where it was.
            usize inside = 0;
            for (const Covered& c : covered) {
                const auto hit = stage.renderer->readPixel(c.x, c.y, view.depth);
                if (hit && hit->id != hidden) {
                    const Vec3 p = worldHit(stage, view, c.x, c.y);
                    inside +=
                        onBoxSurface(p, 0.02) && simd_dot(p - c.at, stage.camera.back()) < -0.01
                            ? 1
                            : 0;
                }
            }
            CHECK(double(inside) >= 0.95 * double(covered.size()));
        }
    }
}

TEST_CASE("seen from behind, a part opened by a hidden face looks as it did") {
    @autoreleasepool {
        Stage stage(box());
        render::ViewState view = stage.view();
        stage.renderer->renderOffscreen(view);
        const auto middle = stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth);
        REQUIRE(middle.has_value());
        const auto target = stage.scene.resolve(middle->id);
        // From the other side, the hidden face is behind the faces in front: nothing changes,
        // though the part now draws both sides.
        stage.camera.lookAt(simd_make_double3(-3, -2, -4), stage.bounds.center(),
                            simd_make_double3(0, 1, 0));
        nav::fit(stage.camera, stage.bounds, stage.viewport, 1.2);
        const std::vector<u8> plain = stage.renderer->renderOffscreen(stage.view());
        stage.state.set({target->globalOccurrence, target->face}, kStateHidden, true);
        const std::vector<u8> hidden = stage.renderer->renderOffscreen(stage.view());
        REQUIRE(plain.size() == hidden.size());
        usize changed = 0;
        for (usize i = 0; i < plain.size(); i += 4) {
            changed += std::abs(int(plain[i]) - int(hidden[i])) > 8 ||
                               std::abs(int(plain[i + 1]) - int(hidden[i + 1])) > 8 ||
                               std::abs(int(plain[i + 2]) - int(hidden[i + 2])) > 8
                           ? 1
                           : 0;
        }
        CHECK(double(changed) < 0.001 * double(plain.size() / 4));
    }
}

TEST_CASE("hiding a face and bringing it back draws the same picture as before") {
    @autoreleasepool {
        Stage stage(box());
        const std::vector<u8> plain = stage.renderer->renderOffscreen(stage.view());
        const render::ViewState view = stage.view();
        const auto middle = stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth);
        REQUIRE(middle.has_value());
        const auto target = stage.scene.resolve(middle->id);
        stage.state.set({target->globalOccurrence, target->face}, kStateHidden, true);
        const std::vector<u8> hidden = stage.renderer->renderOffscreen(stage.view());
        CHECK(hidden != plain);
        stage.state.set({target->globalOccurrence, target->face}, kStateHidden, false);
        CHECK(stage.renderer->renderOffscreen(stage.view()) == plain);
        stage.state.set({target->globalOccurrence, target->face}, kStateHidden, true);
        CHECK(stage.renderer->renderOffscreen(stage.view()) == hidden);
    }
}

namespace {

// A closed tube along z, radii 1 and 0.8, from z = -1 to 1, in `segments` facets, wound outward:
// four faces once imported, its outer wall, its bore and its two ends.
std::vector<float> tube(int segments) {
    std::vector<float> c;
    const auto at = [](float radius, int i, int n, float z) {
        const float a = 2.0f * float(M_PI) * float(i) / float(n);
        return std::array<float, 3>{radius * std::cos(a), radius * std::sin(a), z};
    };
    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % segments; // the last facet meets the first exactly: closed
        addQuad(c, at(1, i, segments, -1), at(1, j, segments, -1), at(1, j, segments, 1),
                at(1, i, segments, 1));
        addQuad(c, at(0.8f, i, segments, -1), at(0.8f, i, segments, 1), at(0.8f, j, segments, 1),
                at(0.8f, j, segments, -1));
        addQuad(c, at(1, i, segments, 1), at(1, j, segments, 1), at(0.8f, j, segments, 1),
                at(0.8f, i, segments, 1));
        addQuad(c, at(1, i, segments, -1), at(0.8f, i, segments, -1), at(0.8f, j, segments, -1),
                at(1, j, segments, -1));
    }
    return c;
}

} // namespace

TEST_CASE("a tube opened by hiding its outer wall shows the near half of its bore") {
    // The bore's near half faces away from the eye: it is drawn only if neither the rasterizer
    // nor the meshlets' normal cones cull the opened part's back faces. The tube has dozens of
    // meshlets, so the cones are tested.
    for (const bool perspective : {false, true}) {
        @autoreleasepool {
            CAPTURE(perspective);
            Stage stage(tube(512), perspective);
            stage.camera.lookAt(simd_make_double3(5, 0, 0), simd_make_double3(0, 0, 0),
                                simd_make_double3(0, 0, 1));
            nav::fit(stage.camera, stage.bounds, stage.viewport, 1.2);
            render::ViewState view = stage.view();
            stage.renderer->renderOffscreen(view);
            const auto middle = stage.renderer->readPixel(kWidth / 2, kHeight / 2, view.depth);
            REQUIRE(middle.has_value());
            const auto wall = stage.scene.resolve(middle->id);
            stage.state.set({wall->globalOccurrence, wall->face}, kStateHidden, true);
            view = stage.view();
            stage.renderer->renderOffscreen(view);
            // Across the middle row, within the bore: every hit on the bore's near half.
            usize hits = 0, near = 0;
            for (int x = 0; x < int(kWidth); ++x) {
                if (!stage.renderer->readPixel(x, kHeight / 2, view.depth)) {
                    continue;
                }
                const Vec3 p = worldHit(stage, view, x, kHeight / 2);
                if (std::abs(p.y) > 0.7) {
                    continue; // where the bore meets the ends' edges
                }
                ++hits;
                near += p.x > 0.0 && std::abs(std::hypot(p.x, p.y) - 0.8) < 0.02 ? 1 : 0;
            }
            CHECK(hits > 40);
            CHECK(double(near) >= 0.95 * double(hits));
        }
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

namespace {

// The default style with `ghostStyle`, a GhostStyle.
void setGhostStyle(Stage& stage, u32 ghostStyle) {
    render::LookSettings settings;
    settings.ghostStyle = ghostStyle;
    stage.renderer->setStyle(
        render::makeStyleUniforms(render::styleNamed(settings.style), settings, 2.0f), false);
}

float luminance(const Stage& stage, const std::vector<u8>& bgra, u32 x, u32 y) {
    const std::array<u8, 3> rgb = stage.pixel(bgra, int(x), int(y));
    return 0.2126f * float(rgb[0]) + 0.7152f * float(rgb[1]) + 0.0722f * float(rgb[2]);
}

} // namespace

// Ghosting a part as dashed lines only leaves it with no skin, but a face ghosted alone that way
// would look exactly hidden: so it keeps a skin, whatever the ghost style.
TEST_CASE("a face ghosted alone keeps a skin in every ghost style, so it never looks hidden") {
    @autoreleasepool {
        Stage stage(box());
        render::ViewState view = stage.view();
        const std::vector<u8> plain = stage.renderer->renderOffscreen(view);
        const auto target = stage.scene.resolve(*visibleIds(stage, view).begin());
        REQUIRE(target.has_value());
        const model::Target face{target->globalOccurrence, target->face};
        for (const u32 ghostStyle : {u32(kGhostShell), u32(kGhostLines), u32(kGhostBoth)}) {
            CAPTURE(ghostStyle);
            setGhostStyle(stage, ghostStyle);
            stage.state.set(face, kStateHidden, true);
            view = stage.view();
            const std::vector<u8> hidden = stage.renderer->renderOffscreen(view);
            stage.state.set(face, kStateHidden, false);
            stage.state.set(face, kStateGhost, true);
            view = stage.view();
            const std::vector<u8> ghosted = stage.renderer->renderOffscreen(view);
            stage.state.set(face, kStateGhost, false);
            // Where the face was, the ghost's skin lies over the inside the gap shows.
            usize covered = 0, skinned = 0;
            for (u32 y = 0; y < kHeight; ++y) {
                for (u32 x = 0; x < kWidth; ++x) {
                    const float gap = luminance(stage, hidden, x, y);
                    if (std::abs(luminance(stage, plain, x, y) - gap) > 5.0f) {
                        ++covered;
                        skinned += std::abs(luminance(stage, ghosted, x, y) - gap) > 5.0f ? 1 : 0;
                    }
                }
            }
            REQUIRE(covered > 1000);
            CAPTURE(covered);
            CHECK(double(skinned) / double(covered) > 0.5);
        }
    }
}

// What is behind a part ghosted as dashed lines only is drawn as if the part were hidden: no skin,
// and its outlines not dimmed. Only the ghost's own dashes differ.
TEST_CASE("a part ghosted whole as lines has no skin and dims nothing behind it") {
    @autoreleasepool {
        std::vector<float> behind = box();
        for (usize i = 0; i < behind.size(); i += 3) {
            behind[i] -= 2.0f;
            behind[i + 1] -= 1.0f;
            behind[i + 2] -= 3.4f;
        }
        Stage stage(std::vector<std::vector<float>>{box(), behind});
        setGhostStyle(stage, kGhostLines);
        render::ViewState view = stage.view();
        const std::vector<u8> plain = stage.renderer->renderOffscreen(view);
        const model::Target front{0, model::Target::kWholeOccurrence};
        stage.state.set(front, kStateHidden, true);
        view = stage.view();
        const std::vector<u8> hidden = stage.renderer->renderOffscreen(view);
        stage.state.set(front, kStateHidden, false);
        stage.state.set(front, kStateGhost, true);
        view = stage.view();
        const std::vector<u8> ghosted = stage.renderer->renderOffscreen(view);

        // Over where the front part was: how much differs at all, and how much of the ink behind
        // it is as dark as with the part hidden.
        usize footprint = 0, changed = 0, inkBehind = 0, inkKept = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const float gap = luminance(stage, hidden, x, y);
                if (std::abs(luminance(stage, plain, x, y) - gap) <= 5.0f) {
                    continue;
                }
                ++footprint;
                const float seen = luminance(stage, ghosted, x, y);
                changed += std::abs(seen - gap) > 5.0f ? 1 : 0;
                if (gap < 100.0f) {
                    ++inkBehind;
                    inkKept += seen < gap + 10.0f ? 1 : 0;
                }
            }
        }
        REQUIRE(footprint > 1000);
        REQUIRE(inkBehind > 50);
        CAPTURE(footprint);
        CAPTURE(inkBehind);
        // Its own dashes and outline change about a quarter of it here; a skin would change it all.
        CHECK(double(changed) / double(footprint) < 0.4);
        CHECK(double(inkKept) / double(inkBehind) > 0.9);
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

TEST_CASE("the view cube is drawn over the model and can be picked") {
    @autoreleasepool {
        Stage stage(box());
        const render::ViewState view = stage.view();
        const render::Overlay overlay =
            render::makeOverlay(stage.camera, stage.viewport, view.uniforms.viewRotation, 1.0f,
                                stage.renderer->glyphAdvance(), 0);
        stage.renderer->renderOffscreen(view, &overlay);
        const simd_float2 center = overlay.uniforms.cubeCenter;
        const auto hit = stage.renderer->readPixel(int(center.x), int(center.y), view.depth);
        REQUIRE(hit.has_value());
        CHECK(hit->id >= kCubeIdBase);
        CHECK(hit->id < kCubeIdBase + 6);
    }
}

TEST_CASE("the see-through shell blends once, even where its faces coincide") {
    @autoreleasepool {
        // The same box twice in one body: every front face has a twin at exactly its depth.
        std::vector<float> doubled = box();
        const std::vector<float> again = box();
        doubled.insert(doubled.end(), again.begin(), again.end());

        const std::array<u32, 3> palette = {0xFF29B8F4u, 0xFF1F9CD4u, 0xFF146D9Au};
        const std::array<u16, 1> shell = {u16(kLookShell)};
        const auto seeThrough = [&](Stage& stage) {
            stage.renderer->syncLooks(shell, palette);
            render::ViewState view = stage.view();
            view.uniforms.seeThrough = 1.0f;
            return stage.renderer->renderOffscreen(view);
        };
        // Edges are left out: the doubled mesh's sides pair differently, which is not what this
        // checks.
        setenv("GEOMETRYVIEW_NO_EDGES", "1", 1);
        Stage single(box());
        Stage twin(doubled);
        unsetenv("GEOMETRYVIEW_NO_EDGES");
        const std::vector<u8> once = seeThrough(single);
        const std::vector<u8> twice = seeThrough(twin);
        usize differing = 0, covered = 0;
        for (u32 y = 0; y < kHeight; y += 4) {
            for (u32 x = 0; x < kWidth; x += 4) {
                const auto a = single.pixel(once, int(x), int(y));
                const auto b = twin.pixel(twice, int(x), int(y));
                covered += single.renderer->readPixel(int(x), int(y), single.view().depth) ? 1 : 0;
                for (usize c = 0; c < 3; ++c) {
                    differing += std::abs(int(a[c]) - int(b[c])) > 2 ? 1 : 0;
                }
            }
        }
        CHECK(covered > 100);
        CHECK(differing == 0);
    }
}
