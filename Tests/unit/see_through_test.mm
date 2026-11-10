// Which parts see-through fades, found by the core on the real GPU. Separate assemblies side by
// side must each have their housings found, as one would alone: neighbours hiding each other, or
// one camera framing them all, must not decide it. Needs the OCCT worker, since the parts are STEP.
//
// GEOMETRYVIEW_SHELL_FILES, cache files separated by colons, prints each one's parts and what the
// test measured of them, for tuning on real models. GEOMETRYVIEW_SEQUENCE_FILES, two cache files,
// runs see-through on and off and a switch between the files, and prints how far each picture is
// from a fresh open (GEOMETRYVIEW_SEQUENCE_OUT keeps the pictures).
#if defined(GEO_TEST_WORKER_PATH)

#include "core/core.h"
#include "core/host.h"
#include "engine/png.h"
#include "gpu/renderer.h"
#include "scene/segment.h"
#include "step/step_import.h"
#include "step_fixture.h"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace geo;

namespace {

// A host that imports nothing itself: the test hands the core a cache file's segments.
class QuietHost final : public engine::Host {
public:
    double now() const override { return clock += 0.001; }
    bool reduceMotion() const override { return true; }
    void runFrames(bool) override {}
    void scheduleRefinement(double) override {}
    void startImport(u64 token, const std::string&) override { imports.push_back(token); }
    void startRefinement(u64, const std::string&, u32, std::vector<u32>) override {}
    void cancelImport(u64) override {}
    void cancelRefinement() override {}
    void evictCache(std::vector<std::string>) override {}
    void emit(const GeoEvent&) override {}
    void publishModel(u32, std::shared_ptr<scene::Segment>) override {}
    void clearModels() override {}
    void publishSwatches(std::vector<u32>, std::vector<u32>) override {}
    void publishNodeStates(std::vector<std::vector<model::NodeVisibility>>) override {}
    void publishSummary(const engine::Summary&) override {}

    std::vector<u64> imports;
    mutable double clock = 0.0;
};

struct Classified {
    std::vector<std::string> names; // each occurrence's, in global order
    std::vector<u8> shells;
};

// Opens a cache file in a core with the Metal renderer, turns see-through on, and returns which
// occurrences it found to be the shell.
Classified classify(const std::string& blob) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const bool hasDevice = device != nil;
    REQUIRE(hasDevice);
    std::unique_ptr<gpu::Renderer> renderer =
        gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
    REQUIRE(renderer != nullptr);
    QuietHost host;
    engine::Core core(*renderer, host);
    core.resize(800, 600, 2.0f);
    core.openFile(blob, false);
    REQUIRE(host.imports.size() == 1);
    const int fd = open(blob.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
    close(fd);
    Classified out;
    for (const std::shared_ptr<scene::Segment>& segment : segments) {
        if (segment->view().hasModel()) {
            const format::SegmentView& model = segment->view();
            for (const format::OccurrenceRecord& o : model.occurrences) {
                const format::NodeRecord& node = model.nodes[o.node];
                out.names.emplace_back(model.strings.data() + node.nameOffset, node.nameLength);
            }
        }
        core.addSegment(host.imports[0], segment);
    }
    core.importFinished(host.imports[0], {true, true, "", blob});
    core.perform(GeoActionToggleSeeThrough);
    out.shells = core.shells();
    return out;
}

std::string importBoxes(test::StepWriter& writer) {
    const std::string text = writer.text();
    char path[] = "/tmp/geoshellXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::StepOptions options;
    options.workerPath = GEO_TEST_WORKER_PATH;
    import::StepStats stats;
    std::string error;
    const bool ok = import::importStep({reinterpret_cast<const u8*>(text.data()), text.size()}, fd,
                                       nullptr, options, stats, error);
    close(fd);
    REQUIRE_MESSAGE(ok, error);
    return path;
}

bool shellNamed(const Classified& c, std::string_view name) {
    for (usize i = 0; i < c.names.size(); ++i) {
        if (c.names[i] == name) {
            return i < c.shells.size() && c.shells[i] != 0;
        }
    }
    FAIL("no part named " << name);
    return false;
}

// Places a hollow box at `lo` of size `size`, holding a block a little smaller than its void (the
// middle half on each axis), with a pin on top that hides nothing, so something stays solid. The
// box's name says nothing of what it is, so only what it hides can make it shell.
void addHousing(test::StepWriter& writer, int root, const std::string& suffix,
                std::array<double, 3> lo, double size) {
    const auto place = [&](const std::string& name, std::array<double, 3> a,
                           std::array<double, 3> b, bool cavity) {
        const int shape = writer.box(a, b, cavity);
        writer.place(root, writer.part(name, shape), shape, {0, 0, 0});
    };
    const auto at = [&](double x, double y, double z) {
        return std::array<double, 3>{lo[0] + x * size, lo[1] + y * size, lo[2] + z * size};
    };
    place("outer" + suffix, at(0, 0, 0), at(1, 1, 1), true);
    place("block" + suffix, at(0.27, 0.27, 0.27), at(0.73, 0.73, 0.73), false);
    place("pin" + suffix, at(0.45, 0.45, 1), at(0.55, 0.55, 1.15), false);
}

} // namespace

TEST_CASE("a small assembly beside a large one is found, as it would be alone") {
    @autoreleasepool {
        // The large one is two housings, one inside the other, around a block. Classified with
        // it, the small one's few pixels and small share of the hiding left it solid in the first
        // round, and the large one's inner housing outweighed it in the second.
        test::StepWriter writer;
        const int root = writer.assembly("pair");
        const auto place = [&](const std::string& name, std::array<double, 3> a,
                               std::array<double, 3> b, bool cavity) {
            const int shape = writer.box(a, b, cavity);
            writer.place(root, writer.part(name, shape), shape, {0, 0, 0});
        };
        place("outer0", {0, 0, 0}, {100, 100, 100}, true); // its void: 25..75
        place("inner0", {27, 27, 27}, {73, 73, 73}, true); // its void: 38.5..61.5
        place("block0", {40, 40, 40}, {60, 60, 60}, false);
        place("pin0", {45, 45, 100}, {55, 55, 115}, false);
        addHousing(writer, root, "1", {200, 0, 0}, 10.0);
        const std::string blob = importBoxes(writer);
        const Classified c = classify(blob);
        REQUIRE(c.shells.size() == c.names.size());
        CHECK(shellNamed(c, "outer0"));
        CHECK(shellNamed(c, "outer1"));
        for (const char* solid : {"block0", "pin0", "block1", "pin1"}) {
            CAPTURE(solid);
            CHECK(!shellNamed(c, solid));
        }
        unlink(blob.c_str());
    }
}

TEST_CASE("each of several housings in a row is found") {
    @autoreleasepool {
        test::StepWriter writer;
        const int root = writer.assembly("row");
        for (int i = 0; i < 3; ++i) {
            addHousing(writer, root, std::to_string(i), {50.0 * i, 0, 0}, 40.0);
        }
        const std::string blob = importBoxes(writer);
        const Classified c = classify(blob);
        REQUIRE(c.shells.size() == c.names.size());
        for (int i = 0; i < 3; ++i) {
            CAPTURE(i);
            CHECK(shellNamed(c, "outer" + std::to_string(i)));
            CHECK(!shellNamed(c, "block" + std::to_string(i)));
            CHECK(!shellNamed(c, "pin" + std::to_string(i)));
        }
        unlink(blob.c_str());
    }
}

TEST_CASE("a housing in a sub-assembly bolted to a larger machine is found, as it would be alone") {
    @autoreleasepool {
        // A belt axis's shape: a plate around a rail around a bar, which between them do nearly
        // all the hiding, and a motor of its own, hanging under the plate and touching it: a
        // stator around a rotor, with a shaft below. Judged beside the whole axis, the stator's
        // share of the hiding was too small in both rounds: the plate's in the first, the rail's
        // in the second. No name here says what anything is.
        test::StepWriter writer;
        const int root = writer.assembly("axis");
        const auto place = [&](int parent, const std::string& name, std::array<double, 3> a,
                               std::array<double, 3> b, bool cavity) {
            const int shape = writer.box(a, b, cavity);
            writer.place(parent, writer.part(name, shape), shape, {0, 0, 0});
        };
        // Each void is the middle half of its box: the plate's 150..450, 50..150, 30..90.
        place(root, "plate", {0, 0, 0}, {600, 200, 120}, true);
        place(root, "rail", {155, 55, 35}, {445, 145, 85}, true);
        place(root, "bar", {230, 80, 50}, {370, 120, 70}, false);
        const int motor = writer.assembly("motor");
        writer.place(root, motor, writer.representationOf(motor), {0, 0, 0});
        place(motor, "stator", {275, 75, -50}, {325, 125, 0}, true);
        place(motor, "rotor", {289, 89, -36}, {311, 111, -14}, false);
        place(motor, "spindle", {297, 97, -70}, {303, 103, -50}, false);
        const std::string blob = importBoxes(writer);
        const Classified c = classify(blob);
        REQUIRE(c.shells.size() == c.names.size());
        CHECK(shellNamed(c, "plate"));
        CHECK(shellNamed(c, "stator"));
        for (const char* solid : {"bar", "rotor", "spindle"}) {
            CAPTURE(solid);
            CHECK(!shellNamed(c, solid));
        }
        unlink(blob.c_str());
    }
}

TEST_CASE("unnamed bodies go by their part's name, so planet gears never fade") {
    @autoreleasepool {
        // Two gears, touching, as the unnamed bodies of one part, each around a pin: by what
        // they hide they are shell, as a housing would be. Only the part's name says they are
        // the mechanism. A housing apart from them is found, so something is.
        test::StepWriter writer;
        const int root = writer.assembly("stage");
        const auto put = [&](const std::string& name, std::vector<test::StepWriter::Solid> solids) {
            const int shape = writer.boxes(solids);
            writer.place(root, writer.part(name, shape), shape, {0, 0, 0});
        };
        put("planet_gears",
            {{{0, 0, 0}, {40, 40, 40}, true, ""}, {{40, 0, 0}, {80, 40, 40}, true, ""}});
        put("pins",
            {{{12, 12, 12}, {28, 28, 28}, false, ""}, {{52, 12, 12}, {68, 28, 28}, false, ""}});
        put("hub", {{{30, 15, 40}, {50, 25, 50}, false}});
        addHousing(writer, root, "1", {300, 0, 0}, 40.0);
        const std::string blob = importBoxes(writer);
        const Classified c = classify(blob);
        REQUIRE(c.shells.size() == c.names.size());
        CHECK(shellNamed(c, "outer1"));
        for (usize i = 0; i < c.names.size(); ++i) {
            CAPTURE(i);
            CAPTURE(c.names[i]);
            // The gears' and pins' bodies are the unnamed ones.
            if (c.names[i].empty()) {
                CHECK(c.shells[i] == 0);
            }
        }
        unlink(blob.c_str());
    }
}

TEST_CASE("where nothing covers anything, the whole file turns to glass") {
    @autoreleasepool {
        // A row of nuts, as lone parts: no group at all.
        {
            test::StepWriter writer;
            const int root = writer.assembly("row");
            for (int i = 0; i < 3; ++i) {
                const int shape = writer.box({50.0 * i, 0, 0}, {50.0 * i + 20, 20, 10}, i == 1);
                writer.place(root, writer.part("nut" + std::to_string(i), shape), shape, {0, 0, 0});
            }
            const std::string blob = importBoxes(writer);
            const Classified c = classify(blob);
            REQUIRE(c.shells.size() == 3);
            CHECK(std::all_of(c.shells.begin(), c.shells.end(), [](u8 s) { return s != 0; }));
            unlink(blob.c_str());
        }
        // Two blocks stacked, which touch but hide nothing inside each other, and one apart: a
        // group, classified, in which nothing is found.
        {
            test::StepWriter writer;
            const int root = writer.assembly("stack");
            const auto place = [&](const std::string& name, std::array<double, 3> a,
                                   std::array<double, 3> b) {
                const int shape = writer.box(a, b);
                writer.place(root, writer.part(name, shape), shape, {0, 0, 0});
            };
            place("lower", {0, 0, 0}, {40, 40, 10});
            place("upper", {10, 10, 10}, {30, 30, 20});
            place("apart", {100, 0, 0}, {110, 10, 10});
            const std::string blob = importBoxes(writer);
            const Classified c = classify(blob);
            REQUIRE(c.shells.size() == 3);
            CHECK(std::all_of(c.shells.begin(), c.shells.end(), [](u8 s) { return s != 0; }));
            unlink(blob.c_str());
        }
    }
}

TEST_CASE("where anything covers something, a loose part beside it stays solid") {
    @autoreleasepool {
        test::StepWriter writer;
        const int root = writer.assembly("bench");
        addHousing(writer, root, "0", {0, 0, 0}, 40.0);
        const int shape = writer.box({100, 0, 0}, {110, 10, 10});
        writer.place(root, writer.part("loose", shape), shape, {0, 0, 0});
        const std::string blob = importBoxes(writer);
        const Classified c = classify(blob);
        REQUIRE(c.shells.size() == c.names.size());
        CHECK(shellNamed(c, "outer0"));
        CHECK(!shellNamed(c, "loose"));
        CHECK(!shellNamed(c, "block0"));
        unlink(blob.c_str());
    }
}

// A core with the Metal renderer, opening cache files as the app does, and drawing what it shows.
struct Viewer {
    std::unique_ptr<gpu::Renderer> renderer;
    QuietHost host;
    std::unique_ptr<engine::Core> core;

    Viewer() {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool hasDevice = device != nil;
        REQUIRE(hasDevice);
        renderer = gpu::Renderer::create(device, GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);
        core = std::make_unique<engine::Core>(*renderer, host);
        core->resize(640, 480, 2.0f);
        renderer->resize(640, 480);
    }

    void open(const std::string& blob, bool add = false) {
        core->openFile(blob, add);
        const u64 token = host.imports.back();
        const int fd = ::open(blob.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        std::vector<std::shared_ptr<scene::Segment>> segments;
        std::string error;
        REQUIRE_MESSAGE(scene::openCacheFile(fd, segments, error), error);
        close(fd);
        for (const std::shared_ptr<scene::Segment>& segment : segments) {
            core->addSegment(token, segment);
        }
        core->importFinished(token, {true, true, "", blob});
    }

    // What the core shows now, as the app's frame would draw it.
    std::vector<u8> picture() {
        core->requestFrame();
        std::optional<engine::Frame> frame;
        for (int i = 0; i < 4 && !frame; ++i) {
            frame = core->frame();
        }
        REQUIRE(frame.has_value());
        // A frame or two more, so any fade has finished.
        for (int i = 0; i < 3; ++i) {
            core->requestFrame();
            if (std::optional<engine::Frame> later = core->frame()) {
                frame = later;
            }
        }
        return renderer->renderOffscreen(frame->view, nullptr);
    }
};

// Mean channel difference, and the share of pixels more than 12 levels apart in any channel.
std::pair<double, double> difference(const std::vector<u8>& a, const std::vector<u8>& b) {
    REQUIRE(a.size() == b.size());
    u64 total = 0, changed = 0;
    for (usize p = 0; p < a.size() / 4; ++p) {
        int worst = 0;
        for (usize c = 0; c < 3; ++c) {
            const int d = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
            total += u64(d);
            worst = std::max(worst, d);
        }
        changed += worst > 12 ? 1 : 0;
    }
    return {double(total) / double(a.size() / 4 * 3), double(changed) / double(a.size() / 4)};
}

TEST_CASE("glass is drawn, and goes again with T") {
    @autoreleasepool {
        // A lone nut-like part: hollow, with nothing else in the file.
        test::StepWriter writer;
        const int root = writer.assembly("nut");
        const int shape = writer.box({0, 0, 0}, {40, 40, 20}, true);
        writer.place(root, writer.part("nut", shape), shape, {0, 0, 0});
        const std::string blob = importBoxes(writer);
        Viewer v;
        v.open(blob);
        const std::vector<u8> solid = v.picture();
        v.core->perform(GeoActionToggleSeeThrough);
        const std::vector<u8> glass = v.picture();
        v.core->perform(GeoActionToggleSeeThrough);
        const std::vector<u8> again = v.picture();
        const auto [glassMean, glassChanged] = difference(glass, solid);
        CAPTURE(glassMean);
        CHECK(glassChanged > 0.02); // the part's faces, a fair share of the view, turned faint
        const auto [againMean, againChanged] = difference(again, solid);
        CHECK(againChanged == 0.0);
        unlink(blob.c_str());
    }
}

// A file opened after another shows its own hidden and ghosted parts, never the other's. A new
// file's face state once started counting its versions again, so when it reached the number the
// last file's upload had, the renderer took it for unchanged: the new file was drawn with the last
// one's hidden faces, at the same indices. Whether that happens depends on how many changes the
// first file had, so the check tries several.
TEST_CASE("a file opened after another is drawn with its own face state") {
    @autoreleasepool {
        test::StepWriter rowWriter;
        const int row = rowWriter.assembly("row");
        for (int i = 0; i < 4; ++i) {
            addHousing(rowWriter, row, std::to_string(i), {50.0 * i, 0, 0}, 40.0);
        }
        const std::string a = importBoxes(rowWriter);
        test::StepWriter pairWriter;
        const int pair = pairWriter.assembly("pair");
        for (int i = 0; i < 2; ++i) {
            addHousing(pairWriter, pair, std::to_string(i), {0, 60.0 * i, 0}, 50.0);
        }
        const std::string b = importBoxes(pairWriter);

        Viewer freshB;
        freshB.open(b);
        const std::vector<u8> expected = freshB.picture();
        for (int changes = 0; changes < 8; ++changes) {
            CAPTURE(changes);
            Viewer v;
            v.open(a);
            v.picture();
            // Hides and shows in turn, ending with parts hidden for odd counts.
            for (int c = 0; c < changes; ++c) {
                v.core->nodeAction(0, 1 + u32(c / 2), c % 2 == 0 ? GeoNodeHide : GeoNodeShow);
                v.picture();
            }
            v.core->nodeAction(0, 1, GeoNodeHide);
            v.core->nodeAction(0, 2, GeoNodeGhost);
            v.picture();
            v.open(b);
            const auto [mean, changed] = difference(v.picture(), expected);
            CHECK(changed < 0.002);
        }
        unlink(a.c_str());
        unlink(b.c_str());
    }
}

TEST_CASE("the user's sequence on the files in GEOMETRYVIEW_SEQUENCE_FILES") {
    const char* files = std::getenv("GEOMETRYVIEW_SEQUENCE_FILES");
    if (files == nullptr) {
        return;
    }
    @autoreleasepool {
        const std::string list(files);
        const std::string a = list.substr(0, list.find(':')), b = list.substr(list.find(':') + 1);
        const char* out = std::getenv("GEOMETRYVIEW_SEQUENCE_OUT");
        const auto save = [&](const char* name, const std::vector<u8>& pixels) {
            if (out != nullptr) {
                engine::writePng((std::string(out) + "/" + name + ".png").c_str(), pixels, 640,
                                 480);
            }
        };
        Viewer fresh;
        fresh.open(a);
        const std::vector<u8> plain = fresh.picture();
        fresh.core->perform(GeoActionToggleSeeThrough);
        const std::vector<u8> through = fresh.picture();
        save("fresh-plain", plain);
        save("fresh-through", through);

        Viewer v;
        v.open(a);
        v.core->perform(GeoActionToggleSeeThrough);
        save("on", v.picture());
        v.core->perform(GeoActionToggleSeeThrough);
        const std::vector<u8> off = v.picture();
        save("off", off);
        const auto [offMean, offChanged] = difference(off, plain);
        std::println("T on then off, against before T: mean {:.2f}, changed {:.4f}", offMean,
                     offChanged);
        v.core->perform(GeoActionToggleSeeThrough);
        v.open(b);
        save("b", v.picture());
        v.open(a);
        const std::vector<u8> back = v.picture();
        save("back", back);
        const auto [backMean, backChanged] = difference(back, through);
        const auto [backPlainMean, backPlainChanged] = difference(back, plain);
        std::println("back to A: against fresh with T mean {:.2f} changed {:.4f}; against fresh "
                     "without mean {:.2f} changed {:.4f}",
                     backMean, backChanged, backPlainMean, backPlainChanged);
    }
}

TEST_CASE("the shells of the files in GEOMETRYVIEW_SHELL_FILES") {
    const char* files = std::getenv("GEOMETRYVIEW_SHELL_FILES");
    if (files == nullptr) {
        return;
    }
    @autoreleasepool {
        std::string_view rest(files);
        while (!rest.empty()) {
            const usize colon = rest.find(':');
            const std::string blob(rest.substr(0, colon));
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
            const Classified c = classify(blob);
            std::println("{}: {} parts", blob, c.names.size());
            for (usize i = 0; i < c.names.size(); ++i) {
                if (i < c.shells.size() && c.shells[i] != 0) {
                    std::println("  shell {}", c.names[i]);
                }
            }
        }
    }
}

#endif
