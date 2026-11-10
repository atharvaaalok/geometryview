// Cuts through parts that touch, overlap and enclose a void, on the real GPU. A cut face must
// belong to the part the plane lies inside and lie on the plane itself, however the parts behind
// it meet. Before caps were found by counting each part's surfaces behind the cut, a part touching
// another from behind fought its cut face, and one reaching into it from behind took it over.
// Needs the OCCT worker, since the parts are STEP.
#if defined(GEO_TEST_WORKER_PATH)

#include "cut_fixture.h"
#include "gpu/renderer.h"
#include "nav/camera.h"
#include "render/view.h"
#include "scene/scene.h"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

using namespace geo;

namespace {

constexpr u32 kSize = 320;
constexpr double kPlaneZ = 2.0;

using Box = test::CutBox;

// The boxes, cut at z = kPlaneZ with everything above removed, seen from above and from the low x
// side: rays through the cut run toward +x and down, so a part's far wall on the +x side is what
// the old caps stood on.
struct Cut {
    std::unique_ptr<gpu::Renderer> renderer;
    nav::Viewport viewport{double(kSize), double(kSize)};
    nav::Camera camera;
    scene::Scene scene;
    model::FaceState state;
    Aabb bounds{};
    render::ViewState view;

    explicit Cut(const std::vector<Box>& boxes) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool hasDevice = device != nil;
        REQUIRE(hasDevice);
        renderer = gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);
        const std::string blob = test::importBoxes(boxes, GEO_TEST_WORKER_PATH);
        const int fd = open(blob.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<std::shared_ptr<scene::Segment>> segments;
        std::string error;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        unlink(blob.c_str());
        scene::Document& document = scene.document(scene.addDocument());
        for (std::shared_ptr<scene::Segment>& segment : segments) {
            REQUIRE_MESSAGE(document.add(std::move(segment), error), error);
        }
        renderer->syncScene(scene);
        state.resize(scene.faceCounts());
        renderer->syncState(state);
        renderer->resize(kSize, kSize);
        bounds = scene.bounds().value();

        camera.perspective = false;
        const Vec3 center = bounds.center();
        camera.lookAt(center + simd_make_double3(-5.0, -3.0, 9.0), center,
                      simd_make_double3(0, 0, 1));
        nav::fit(camera, bounds, viewport, 1.1);
        view = render::makeView(camera, viewport, bounds);
        render::Section section;
        section.enabled[2] = true;
        section.position[2] = float((kPlaneZ - bounds.min.z) / (bounds.max.z - bounds.min.z));
        render::applySection(section, bounds, view.uniforms);
        renderer->renderOffscreen(view);
    }

    double ndcX(int x) const { return 2.0 * (double(x) + 0.5) / kSize - 1.0; }
    double ndcY(int y) const { return 1.0 - 2.0 * (double(y) + 0.5) / kSize; }

    // Where the pixel's view ray crosses the plane.
    Vec3 onPlane(int x, int y) const {
        const Vec3 near = camera.unproject(ndcX(x), ndcY(y), 0.0, viewport);
        const Vec3 far = camera.unproject(ndcX(x), ndcY(y), 1.0, viewport);
        const double t = (kPlaneZ - near.z) / (far.z - near.z);
        return near + (far - near) * t;
    }

    struct Seen {
        u32 occurrence;
        Vec3 world;
    };

    std::optional<Seen> seen(int x, int y) const {
        const std::optional<gpu::PixelHit> hit = renderer->readPixel(x, y, view.depth);
        if (!hit.has_value() || hit->id == 0) {
            return std::nullopt;
        }
        const std::optional<scene::PickTarget> target = scene.resolve(hit->id);
        if (!target.has_value()) {
            return std::nullopt;
        }
        return Seen{target->globalOccurrence,
                    camera.unproject(ndcX(x), ndcY(y), hit->viewDepth, viewport)};
    }

    // Every few pixels whose point on the plane passes `where`, kept a few pixels clear of its
    // edges by `margin`, in world units.
    void forEach(const std::function<bool(Vec3, double)>& where,
                 const std::function<void(int, int, Vec3)>& check) const {
        const double margin = 3.0 * double(view.uniforms.worldPerPixel);
        for (int y = 2; y < int(kSize) - 2; y += 3) {
            for (int x = 2; x < int(kSize) - 2; x += 3) {
                const Vec3 p = onPlane(x, y);
                if (where(p, margin)) {
                    check(x, y, p);
                }
            }
        }
    }
};

bool inside(const Box& box, Vec3 p, double margin) {
    for (int axis = 0; axis < 2; ++axis) {
        if (p[axis] < box.lo[axis] + margin || p[axis] > box.hi[axis] - margin) {
            return false;
        }
    }
    return true;
}

// Checks that every pixel whose plane point lies inside `box` shows one part, the same as at the
// box's middle, on the plane. Returns that part.
u32 checkCutFace(
    const Cut& cut, const Box& box,
    const std::function<bool(Vec3, double)>& also = [](Vec3, double) { return true; }) {
    std::optional<u32> owner;
    usize checked = 0, wrongPart = 0, offPlane = 0;
    cut.forEach([&](Vec3 p, double margin) { return inside(box, p, margin) && also(p, margin); },
                [&](int x, int y, Vec3) {
                    const std::optional<Cut::Seen> seen = cut.seen(x, y);
                    REQUIRE(seen.has_value());
                    owner = owner.value_or(seen->occurrence);
                    ++checked;
                    wrongPart += seen->occurrence != *owner ? 1 : 0;
                    offPlane += std::abs(seen->world.z - kPlaneZ) > 0.02 ? 1 : 0;
                });
    CAPTURE(checked);
    REQUIRE(checked > 100);
    CHECK(wrongPart == 0);
    CHECK(offPlane == 0);
    return owner.value_or(0);
}

} // namespace

TEST_CASE("a cut face is its own part's, on the plane, where another part touches it from behind") {
    @autoreleasepool {
        // Side by side along x: the left box's far wall is the right box's near one.
        const Box left{{0, 0, 0}, {4, 4, 4}}, right{{4, 0, 0}, {8, 4, 4}};
        const Cut cut({left, right});
        const u32 leftPart = checkCutFace(cut, left);
        const u32 rightPart = checkCutFace(cut, right);
        CHECK(leftPart != rightPart);
    }
}

TEST_CASE("a part reaching into another from behind its cut does not take the cut face") {
    @autoreleasepool {
        // The small box's bottom is the nearest far wall along those rays.
        const std::vector<Box> boxes = test::reachingBoxes();
        const Cut cut(boxes);
        checkCutFace(cut, boxes[0]);
    }
}

TEST_CASE("a void inside a cut part shows its walls, and only the part around it is a cut face") {
    @autoreleasepool {
        // The cavity is the middle half of the box on each axis, from 2 to 6; the plane at z = 2
        // is its floor, so the plane is lifted to the box's middle here.
        const Box hollow{{0, 0, -2}, {8, 8, 6}, true};
        const Box cavity{{2, 2, 0}, {6, 6, 4}};
        const Cut cut({hollow});
        // The wall around the cavity is cut on the plane.
        checkCutFace(cut, hollow, [&](Vec3 p, double margin) {
            return !inside({{cavity.lo[0] - margin, cavity.lo[1] - margin, 0},
                            {cavity.hi[0] + margin, cavity.hi[1] + margin, 0}},
                           p, 0.0);
        });
        // Through the cavity the rays reach its walls and floor, below the plane.
        usize checked = 0, onPlane = 0;
        cut.forEach([&](Vec3 p, double margin) { return inside(cavity, p, margin); },
                    [&](int x, int y, Vec3) {
                        const std::optional<Cut::Seen> seen = cut.seen(x, y);
                        REQUIRE(seen.has_value());
                        ++checked;
                        onPlane += seen->world.z > kPlaneZ - 0.05 ? 1 : 0;
                    });
        REQUIRE(checked > 100);
        CHECK(onPlane == 0);
    }
}

#endif
