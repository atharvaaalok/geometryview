// The engine core driven as a platform drives it, through a fake renderer and a fake host that
// record what they are asked. Runs natively and as WebAssembly, since the core is portable.
#include "cache/blob_writer.h"
#include "core/core.h"
#include "import.h"
#include "scene/segment.h"

#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace geo;
using format::NodeKind;

namespace {

class FakeRenderer final : public render::Backend {
public:
    void syncScene(const scene::Scene&) override { ++sceneSyncs; }
    void syncState(const model::FaceState&) override {}
    void setStyle(const StyleUniforms&, bool) override {}
    void syncLooks(std::span<const u16>, std::span<const u32>) override {}
    void setShellGroups(std::span<const u32>) override {}
    void setReducedDetail(bool reduced) override { reducedDetail = reduced; }
    u64 sceneTriangles() const override { return 0; }
    u64 sceneEdgeSegments() const override { return 0; }
    void resize(u32 w, u32 h) override {
        width = w;
        height = h;
    }
    float glyphAdvance() const override { return 16.0f; }
    std::optional<render::PixelHit> readPixel(int, int,
                                              const render::DepthDecoder&) const override {
        return std::nullopt;
    }
    render::OccurrenceIds renderOccurrenceIds(const render::ViewState&, u32 size) override {
        return {std::vector<u32>(usize(size) * size), std::vector<u32>(usize(size) * size)};
    }
    // As the browser's renderer does when `deferIds` is set: ids arrive when the test delivers
    // them, frames later.
    void requestOccurrenceIds(const render::ViewState& view, u32 size,
                              std::function<void(render::OccurrenceIds)> done) override {
        if (!deferIds) {
            done(renderOccurrenceIds(view, size));
            return;
        }
        pendingIds.push_back(
            [this, view, size, done = std::move(done)] { done(renderOccurrenceIds(view, size)); });
    }
    // Delivers the oldest `count` requests, or all of them.
    void deliverIds(usize count = ~usize(0)) {
        count = std::min(count, pendingIds.size());
        std::vector<std::function<void()>> arriving(pendingIds.begin(),
                                                    pendingIds.begin() + std::ptrdiff_t(count));
        pendingIds.erase(pendingIds.begin(), pendingIds.begin() + std::ptrdiff_t(count));
        for (const std::function<void()>& deliver : arriving) {
            deliver();
        }
    }
    bool deferIds = false;
    std::vector<std::function<void()>> pendingIds;
    std::vector<u8> renderOffscreen(const render::ViewState&, const render::Overlay* overlay) override {
        ++offscreenRenders;
        offscreenOverlay = overlay != nullptr && overlay->visible;
        return std::vector<u8>(usize(width) * height * 4);
    }

    u32 sceneSyncs = 0, offscreenRenders = 0, width = 0, height = 0;
    bool reducedDetail = false;
    bool offscreenOverlay = false; // whether the last offscreen render had a visible overlay
};

class FakeHost final : public engine::Host {
public:
    double now() const override { return time; }
    bool reduceMotion() const override { return true; } // flights land at once
    void runFrames(bool running) override { framesRunning = running; }
    void scheduleRefinement(double delaySeconds) override {
        refinementDelays.push_back(delaySeconds);
    }
    void startImport(u64 token, const std::string& path) override {
        imports.push_back({token, path});
    }
    void startRefinement(u64 token, const std::string&, u32 level, std::vector<u32>) override {
        refinements.push_back({token, level});
    }
    void cancelImport(u64 token) override { cancelled.push_back(token); }
    void cancelRefinement() override {}
    void evictCache(std::vector<std::string> keep) override { evictions.push_back(keep); }
    void emit(const GeoEvent& event) override {
        events.push_back(event.kind);
        messages.push_back(event.message);
    }
    void publishModel(u32 document, std::shared_ptr<scene::Segment> model) override {
        models.resize(std::max<usize>(models.size(), document + 1));
        models[document] = std::move(model);
    }
    void clearModels() override { models.clear(); }
    void publishSwatches(std::vector<u32> swatches, std::vector<u32>) override {
        swatchCount = u32(swatches.size());
    }
    void publishNodeStates(std::vector<std::vector<model::NodeVisibility>> states) override {
        nodeStates = std::move(states);
    }
    void publishSummary(const engine::Summary& counts) override { summary = counts; }

    bool saw(GeoEventKind kind) const {
        return std::find(events.begin(), events.end(), kind) != events.end();
    }

    double time = 100.0;
    bool framesRunning = false;
    std::vector<double> refinementDelays;
    std::vector<std::pair<u64, std::string>> imports;
    std::vector<std::pair<u64, u32>> refinements;
    std::vector<u64> cancelled;
    std::vector<std::vector<std::string>> evictions;
    std::vector<GeoEventKind> events;
    std::vector<std::string> messages;
    std::vector<std::shared_ptr<scene::Segment>> models;
    std::vector<std::vector<model::NodeVisibility>> nodeStates;
    engine::Summary summary;
    u32 swatchCount = 0;
};

format::NodeRecord node(u32 parent, u32 end, u32 first, u32 last, NodeKind kind) {
    return {parent, end, first, last, 0, 0, u32(kind), 0};
}

// A cache file's segments for the interaction test's rig: part A with two bodies, part B with one,
// each a unit box. Nodes: 0 the root, 1 part A, 2 and 3 its bodies, 4 part B.
std::vector<std::shared_ptr<scene::Segment>> rigSegments() {
    import::ModelData model;
    model.definitions = {{{0, 0, 0}, {1, 1, 1}, 3, 0}, {{0, 0, 0}, {1, 1, 1}, 5, 0}};
    model.occurrences = {{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 0, 2, 0, 0},
                         {{1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 0}, 1, 3, 0, 0},
                         {{1, 0, 0, 0, 1, 0, 0, 0, 1, 20, 0, 0}, 0, 4, 0, 0}};
    model.nodes = {node(format::kNoParent, 5, 0, 3, NodeKind::Assembly),
                   node(0, 4, 0, 2, NodeKind::Part), node(1, 3, 0, 1, NodeKind::Body),
                   node(1, 4, 1, 2, NodeKind::Body), node(0, 5, 2, 3, NodeKind::Part)};
    char path[] = "/tmp/geocoreXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::BlobStream stream(fd, nullptr);
    REQUIRE(stream.writeModel(model));
    REQUIRE(stream.finish({}));
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
    close(fd);
    unlink(path);
    return segments;
}

// A cache file with geometry: two triangles, as the STL importer writes them.
std::vector<std::shared_ptr<scene::Segment>> meshSegments() {
    constexpr std::string_view stl = "solid s\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                                     "vertex 1 0 0\nvertex 1 1 0\nvertex 0 1 0\nendsolid s\n";
    char path[] = "/tmp/geocoremeshXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::ImportResult result;
    std::string error;
    REQUIRE(import::importMesh({reinterpret_cast<const u8*>(stl.data()), stl.size()}, fd, nullptr,
                               result, error));
    std::vector<std::shared_ptr<scene::Segment>> segments;
    REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
    close(fd);
    unlink(path);
    return segments;
}

struct Rig {
    FakeRenderer renderer;
    FakeHost host;
    engine::Core core{renderer, host};

    // Opens the rig as a platform would: the import starts, its segments arrive, it finishes.
    u64 open(bool add = false) {
        core.openFile("/models/rig.step", add);
        const u64 token = host.imports.back().first;
        for (const auto& segment : rigSegments()) {
            core.addSegment(token, segment);
        }
        core.importFinished(token, {true, false, "", "/cache/rig.geoblob"});
        return token;
    }

    // The mesh, and a second copy of it added in the same place: two parts that touch, which
    // see-through classifies as one assembly.
    void openTwoMeshes() {
        openMesh();
        openMesh(true);
    }

    u64 openMesh(bool add = false) {
        core.openFile("/models/mesh.stl", add);
        const u64 token = host.imports.back().first;
        for (const auto& segment : meshSegments()) {
            core.addSegment(token, segment);
        }
        core.importFinished(token, {true, false, "", "/cache/mesh.geoblob"});
        return token;
    }
};

} // namespace

TEST_CASE("opening a file starts its import and announces it by name") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.core.openFile("/models/rig.step", false);
    REQUIRE(rig.host.imports.size() == 1);
    CHECK(rig.host.imports[0].second == "/models/rig.step");
    CHECK(rig.host.saw(GeoEventLoading));
    CHECK(std::find(rig.host.messages.begin(), rig.host.messages.end(), "rig.step") !=
          rig.host.messages.end());
    CHECK(rig.host.framesRunning);
    CHECK(rig.renderer.width == 800);
}

TEST_CASE("a finished import publishes the tree, frames the scene and is ready") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    REQUIRE(rig.host.models.size() == 1);
    CHECK(rig.host.models[0] != nullptr);
    CHECK(rig.host.saw(GeoEventModelReady));
    CHECK(rig.host.saw(GeoEventSceneReady));
    // Freshly imported, so the cache is trimmed, sparing the file just made.
    REQUIRE(rig.host.evictions.size() == 1);
    CHECK(rig.host.evictions[0] == std::vector<std::string>{"/cache/rig.geoblob"});
    // Finer geometry waits until the view has been still a moment.
    CHECK(!rig.host.refinementDelays.empty());
}

TEST_CASE("results for a document that was replaced are ignored") {
    Rig rig;
    rig.core.openFile("/models/first.step", false);
    const u64 first = rig.host.imports.back().first;
    rig.open(); // replaces the first
    CHECK(std::find(rig.host.cancelled.begin(), rig.host.cancelled.end(), first) !=
          rig.host.cancelled.end());
    const usize events = rig.host.events.size();
    for (const auto& segment : rigSegments()) {
        rig.core.addSegment(first, segment);
    }
    rig.core.importFinished(first, {false, false, "late", ""});
    CHECK(rig.host.events.size() == events);
}

TEST_CASE("closing empties the scene, stops the import, and a file opens normally after") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    REQUIRE(rig.core.sceneFaces() > 0);
    // A second file, still importing when everything is closed.
    rig.core.openFile("/models/second.step", true);
    const u64 second = rig.host.imports.back().first;
    rig.core.close();
    CHECK(rig.core.sceneFaces() == 0);
    CHECK(!rig.core.sceneBounds().has_value());
    CHECK(rig.host.models.empty());
    CHECK(std::find(rig.host.cancelled.begin(), rig.host.cancelled.end(), second) !=
          rig.host.cancelled.end());
    CHECK(rig.host.framesRunning); // the empty view is drawn
    // What the closed import still sends changes nothing.
    const usize events = rig.host.events.size();
    for (const auto& segment : rigSegments()) {
        rig.core.addSegment(second, segment);
    }
    rig.core.importFinished(second, {true, false, "", "/cache/second.geoblob"});
    CHECK(rig.host.events.size() == events);
    CHECK(rig.core.sceneFaces() == 0);
    // Opening again works as it did the first time.
    rig.open();
    CHECK(rig.core.sceneFaces() > 0);
    REQUIRE(rig.host.models.size() == 1);
    CHECK(rig.host.models[0] != nullptr);
}

TEST_CASE("tree actions change visibility, publish it, and undo") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    rig.core.nodeAction(0, 1, GeoNodeHide); // part A, both its bodies
    REQUIRE(rig.host.nodeStates.size() == 1);
    CHECK(rig.host.nodeStates[0][1] == model::NodeVisibility::Hidden);
    CHECK(rig.host.summary.hiddenOccurrences == 2);
    CHECK(rig.host.saw(GeoEventVisibilityChanged));
    rig.core.perform(GeoActionUndo);
    CHECK(rig.host.nodeStates[0][1] == model::NodeVisibility::Shown);
    CHECK(rig.host.summary.hiddenOccurrences == 0);
    // An action on a node that does not exist is ignored.
    rig.core.nodeAction(0, 99, GeoNodeHide);
    CHECK(rig.host.summary.hiddenOccurrences == 0);
}

TEST_CASE("frames are drawn only when something changed, and the clock stops when still") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    const std::optional<engine::Frame> first = rig.core.frame();
    REQUIRE(first.has_value());
    CHECK(rig.renderer.sceneSyncs == 1);
    CHECK(first->view.uniforms.viewport.x == doctest::Approx(800.0));
    CHECK(!rig.core.frame().has_value()); // nothing changed
    CHECK(rig.host.framesRunning);
    rig.host.time += 1.0; // still for a second
    CHECK(!rig.core.frame().has_value());
    CHECK(!rig.host.framesRunning);
    rig.core.perform(GeoActionViewTop); // lands at once, motion being reduced
    CHECK(rig.host.framesRunning);
    CHECK(rig.core.frame().has_value());
}

TEST_CASE("a snapshot draws the view offscreen through the backend") {
    Rig rig;
    rig.core.resize(64, 48, 1.0f);
    rig.open();
    const std::vector<u8> pixels = rig.core.snapshot();
    CHECK(rig.renderer.offscreenRenders == 1);
    CHECK(pixels.size() == usize(64) * 48 * 4);
}

TEST_CASE("the view cube and scale bar can be left out, of frames and snapshots") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    const std::optional<engine::Frame> shown = rig.core.frame();
    REQUIRE(shown.has_value());
    CHECK(shown->overlay.visible);
    rig.core.snapshot();
    CHECK(rig.renderer.offscreenOverlay);

    rig.core.setOverlayShown(false);
    const std::optional<engine::Frame> plain = rig.core.frame(); // leaving it out draws a frame
    REQUIRE(plain.has_value());
    CHECK(!plain->overlay.visible);
    rig.core.snapshot();
    CHECK(!rig.renderer.offscreenOverlay);

    rig.core.setOverlayShown(true);
    const std::optional<engine::Frame> again = rig.core.frame();
    REQUIRE(again.has_value());
    CHECK(again->overlay.visible);
}

TEST_CASE("the view moving reschedules refinement, which then draws in full") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.open();
    const usize before = rig.host.refinementDelays.size();
    rig.core.perform(GeoActionFit);
    CHECK(rig.host.refinementDelays.size() == before + 1);
    CHECK(rig.host.refinementDelays.back() == doctest::Approx(0.3));
    // A model without tolerances, as this rig is, has nothing to refine.
    rig.core.refinementDue();
    CHECK(rig.host.refinements.empty());
}

TEST_CASE("the shell is classified from views read back later, and stale ones are dropped") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.renderer.deferIds = true;
    rig.openTwoMeshes();
    rig.core.perform(GeoActionToggleSeeThrough);
    // Fourteen views are asked for at once, and nothing is decided until they are all back.
    CHECK(rig.renderer.pendingIds.size() == 14);
    rig.host.framesRunning = false;
    rig.renderer.deliverIds();
    // No part hides another, so no second round is needed, and the view is drawn again.
    CHECK(rig.renderer.pendingIds.empty());
    CHECK(rig.host.framesRunning);

    // With see-through on, new files are classified when they finish importing. Views of the
    // files they replaced, still in flight, change nothing when they arrive.
    rig.openTwoMeshes();
    REQUIRE(rig.renderer.pendingIds.size() == 14);
    rig.openTwoMeshes();
    REQUIRE(rig.renderer.pendingIds.size() == 28);
    rig.host.framesRunning = false;
    rig.renderer.deliverIds(14);
    CHECK(!rig.host.framesRunning);
    rig.renderer.deliverIds();
    CHECK(rig.host.framesRunning);
}

TEST_CASE("the shell is classified at once by a renderer that can wait for the GPU") {
    Rig rig;
    rig.core.resize(800, 600, 2.0f);
    rig.openMesh();
    rig.host.framesRunning = false;
    rig.core.perform(GeoActionToggleSeeThrough);
    CHECK(rig.renderer.pendingIds.empty());
    CHECK(rig.host.framesRunning);
}
