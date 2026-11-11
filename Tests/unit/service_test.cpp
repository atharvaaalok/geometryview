// The library service: a gallery's files to facts and pictures, through its caches, the helper and
// a painter. The painter here only writes a file, so these run without a GPU.
#include "library/service.h"

#include "drive_fixture.h"
#include "ipc/cache_eviction.h"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace geo;
using library::Band;
using library::FileWant;
namespace fs = std::filesystem;

namespace {

std::string triangleStl(double size) {
    return std::format("solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex {} 0 0\n"
                       "vertex 0 {} 1\nendloop\nendfacet\nendsolid t\n",
                       size, size);
}

struct FakePainter final : library::Painter {
    std::mutex mutex;
    std::vector<std::string> painted; // blob paths, in order
    u32 trims = 0;

    bool paint(const std::string& blobPath, const library::PictureSpec& spec, i32,
               const std::string& pngPath, library::PictureNote& note, std::string&) override {
        std::ofstream(pngPath) << "picture of " << blobPath << " in style " << spec.style;
        note.width = note.height = spec.pixels;
        note.partRect = {1, 2, 3, 4};
        note.inkRect = {0, 0, 10, 12};
        note.worldPerPixel = 0.25;
        std::lock_guard lock(mutex);
        painted.push_back(blobPath);
        return true;
    }
    void trim() override {
        std::lock_guard lock(mutex);
        ++trims;
    }
};

struct Recorder final : library::ServiceListener {
    std::mutex mutex;
    std::map<std::string, std::vector<scene::Facts>> seen;
    std::map<std::string, std::string> pictures; // path -> png
    std::vector<std::string> pictureOrder;
    std::map<std::string, std::string> failures;

    void facts(const std::string& path, const scene::Facts& f) override {
        std::lock_guard lock(mutex);
        seen[path].push_back(f);
    }
    void picture(const std::string& path, const std::string& png,
                 const library::PictureNote& note) override {
        CHECK(note.width > 0);
        std::lock_guard lock(mutex);
        pictures[path] = png;
        pictureOrder.push_back(path);
    }
    void failed(const std::string& path, const std::string& message) override {
        std::lock_guard lock(mutex);
        failures[path] = message;
    }
    bool hasFinalFacts(const std::string& path) {
        std::lock_guard lock(mutex);
        const auto it = seen.find(path);
        return it != seen.end() && !it->second.empty() && !it->second.back().estimated;
    }
};

// A folder of sources and a cache, removed afterwards.
struct Workspace {
    fs::path root;
    library::ServiceConfig config;
    library::ImportGate gate{8};

    Workspace() {
        char pattern[] = "/tmp/geoserviceXXXXXX";
        REQUIRE(mkdtemp(pattern) != nullptr);
        root = pattern;
        fs::create_directories(root / "parts");
        fs::create_directories(root / "cache");
        config.paths = {GEO_TEST_HELPER_PATH, (root / "cache").string()};
        config.galleryDirectory = (root / "cache" / "gallery").string();
        config.slots = 2;
        config.workers = 1;
        config.gate = &gate;
        config.pictureVersion = "test";
    }
    ~Workspace() { fs::remove_all(root); }

    std::string write(const std::string& name, const std::string& contents) const {
        const fs::path path = root / "parts" / name;
        std::ofstream(path) << contents;
        return path.string();
    }

    std::vector<std::string> blobs() const {
        std::vector<std::string> found;
        for (const auto& entry : fs::directory_iterator(root / "cache")) {
            if (entry.path().extension() == ".geoblob") {
                found.push_back(entry.path().string());
            }
        }
        return found;
    }
};

std::vector<FileWant> wantAll(const std::vector<std::string>& paths, Band band = Band::Visible) {
    std::vector<FileWant> wants;
    for (usize i = 0; i < paths.size(); ++i) {
        wants.push_back({paths[i], band, double(i), fs::file_size(paths[i]), -1});
    }
    return wants;
}

std::vector<std::string> someStls(const Workspace& workspace, int count) {
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i) {
        paths.push_back(workspace.write(std::format("t{}.stl", i), triangleStl(1.0 + i)));
    }
    return paths;
}

} // namespace

TEST_CASE("a gallery's files get facts and pictures, and a second service on the same cache "
          "starts no helper") {
    Workspace workspace;
    const std::vector<std::string> paths = someStls(workspace, 4);
    {
        FakePainter painter;
        Recorder recorder;
        library::Service service(workspace.config, painter, recorder);
        service.want(wantAll(paths));
        service.waitIdle();
        for (const std::string& path : paths) {
            CAPTURE(path);
            CHECK(recorder.hasFinalFacts(path));
            CHECK(recorder.pictures.contains(path));
        }
        const scene::Facts& first = recorder.seen[paths[0]].back();
        CHECK(first.triangles == 1);
        CHECK(first.size()[0] == doctest::Approx(1.0));
        const library::Service::Stats stats = service.stats();
        CHECK(stats.helpers == 4);
        CHECK(stats.imports == 4);
        CHECK(stats.pictures == 4);
        CHECK(recorder.failures.empty());
    }
    // The same cache, and a helper that cannot even start: everything comes from the cache.
    workspace.config.paths.helper = (workspace.root / "no-helper").string();
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    service.want(wantAll(paths));
    service.waitIdle();
    for (const std::string& path : paths) {
        CHECK(recorder.hasFinalFacts(path));
        CHECK(recorder.pictures.contains(path));
    }
    CHECK(service.stats().helpers == 0);
    CHECK(service.stats().lookups == 4);
    CHECK(painter.painted.empty());
}

TEST_CASE("a new style draws pictures again from the cache, those in view first") {
    Workspace workspace;
    const std::vector<std::string> paths = someStls(workspace, 4);
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    service.want(wantAll(paths, Band::Near));
    service.waitIdle();
    REQUIRE(service.stats().imports == 4);

    std::vector<FileWant> wants = wantAll(paths, Band::Near);
    wants[3].band = Band::Visible; // the last file is in view now
    service.want(wants);
    recorder.pictureOrder.clear();
    library::PictureSpec blueprint;
    blueprint.style = 3;
    service.setPicture(blueprint);
    service.waitIdle();
    REQUIRE(recorder.pictureOrder.size() == 4);
    CHECK(recorder.pictureOrder.front() == paths[3]);
    CHECK(service.stats().imports == 4); // no file imported again
    CHECK(service.stats().pictures == 8);
}

TEST_CASE("facts and pictures outlive evicted cache files; only a new style imports again") {
    Workspace workspace;
    const std::vector<std::string> paths = someStls(workspace, 3);
    {
        FakePainter painter;
        Recorder recorder;
        library::Service service(workspace.config, painter, recorder);
        service.want(wantAll(paths));
        service.waitIdle();
    }
    for (const std::string& blob : workspace.blobs()) {
        fs::remove(blob);
    }
    {
        FakePainter painter;
        Recorder recorder;
        library::Service service(workspace.config, painter, recorder);
        service.want(wantAll(paths));
        service.waitIdle();
        CHECK(recorder.pictures.size() == 3);
        CHECK(service.stats().helpers == 0);
    }
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    library::PictureSpec studio;
    studio.style = 5;
    service.setPicture(studio);
    service.want(wantAll(paths));
    service.waitIdle();
    CHECK(recorder.pictures.size() == 3);
    CHECK(service.stats().imports == 3);
}

TEST_CASE("a file that cannot be read fails once, and is not tried again until it changes") {
    Workspace workspace;
    const std::string bad = workspace.write("bad.stl", "solid nothing\nendsolid\n");
    const std::string good = workspace.write("good.stl", triangleStl(2.0));
    {
        FakePainter painter;
        Recorder recorder;
        library::Service service(workspace.config, painter, recorder);
        service.want(wantAll({bad, good}));
        service.waitIdle();
        CHECK(recorder.failures.contains(bad));
        CHECK(recorder.pictures.contains(good));
        CHECK(service.stats().failures == 1);
    }
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    service.want(wantAll({bad, good}));
    service.waitIdle();
    CHECK(recorder.failures.contains(bad));
    CHECK(service.stats().helpers == 0);
}

TEST_CASE("a missing file says so") {
    Workspace workspace;
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    const std::string missing = (workspace.root / "parts" / "gone.step").string();
    service.want({{missing, Band::Visible, 0.0, 10, -1}});
    service.waitIdle();
    CHECK(recorder.failures[missing] == "This file could not be found.");
}

namespace {

// Cache files of files the user opened, recently: what a gallery must not evict.
std::vector<std::string> recentBlobs(const Workspace& workspace, int count, usize bytes) {
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i) {
        const fs::path path = workspace.root / "cache" / std::format("{:016x}.geoblob", 0xabc0 + i);
        std::ofstream(path) << std::string(bytes, 'r');
        paths.push_back(path.string());
    }
    return paths;
}

} // namespace

TEST_CASE("prefetching stops while cache files fill most of their budget") {
    Workspace workspace;
    // The recent files fill 71% of the budget, past the 70% prefetching stops at, leaving room
    // for one 48 KB file in view.
    const std::vector<std::string> recent = recentBlobs(workspace, 6, 20000);
    workspace.config.blobBudget = 170000;
    const std::vector<std::string> paths = someStls(workspace, 6);
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    std::vector<FileWant> wants = wantAll(paths, Band::Prefetch);
    wants[0].band = Band::Visible;
    service.want(wants);
    service.waitIdle();
    CHECK(service.stats().imports == 1); // only the file in view
    CHECK(recorder.pictures.contains(paths[0]));
    for (const std::string& path : recent) {
        CHECK(fs::exists(path));
    }
}

TEST_CASE("a prefetch walk evicts the gallery's own cache files, not the files opened recently") {
    Workspace workspace;
    const std::vector<std::string> recent = recentBlobs(workspace, 2, 20000);
    const std::vector<std::string> paths = someStls(workspace, 12);
    // Room for the recent files and a few of the gallery's, of 48 KB each.
    workspace.config.blobBudget = 40000 + 4 * 49152;
    workspace.config.evictionSeconds = 0.0;
    workspace.config.slots = 1;
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    service.want(wantAll(paths, Band::Prefetch));
    service.waitIdle();
    for (const std::string& path : recent) {
        CHECK(fs::exists(path));
    }
    // Each file was drawn before its cache file went.
    CHECK(recorder.pictures.size() >= 3);
    CHECK(service.stats().evictions > 0);
    CHECK(ipc::cacheBytes((workspace.root / "cache").string()) <= workspace.config.blobBudget);
}

TEST_CASE("a file being opened is not kept waiting, and gallery imports not in view make way") {
    Workspace workspace;
    // A helper that takes its time, so the gallery's imports are still running.
    const fs::path slow = workspace.root / "slow-helper.sh";
    std::ofstream(slow) << "#!/bin/sh\nsleep 20\n";
    fs::permissions(slow, fs::perms::owner_all);
    workspace.config.paths.helper = slow.string();
    library::ImportGate gate(2);
    workspace.config.gate = &gate;
    workspace.config.slots = 2;
    workspace.config.workers = 1;
    const std::vector<std::string> paths = someStls(workspace, 4);
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    std::vector<FileWant> wants = wantAll(paths, Band::Near);
    wants[0].band = Band::Visible;
    service.want(wants);
    // Wait for both slots to fill.
    for (int i = 0; i < 200 && gate.held() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(gate.held() == 2);

    const auto start = std::chrono::steady_clock::now();
    library::ImportGate::Ticket opening = gate.open();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5));
    // The import near the view is cancelled; the one in view runs on.
    for (int i = 0; i < 300 && service.stats().cancelled == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(service.stats().cancelled == 1);
    CHECK(gate.held() == 1);
    // Nothing more starts while the file is being opened.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(gate.held() == 1);
    opening.release();
    // The service's destructor cancels the rest.
}

#ifdef GEO_TEST_WORKER_PATH
TEST_CASE("STEP files waiting to import are laid out from their structure first") {
    Workspace workspace;
    workspace.config.slots = 1;
    std::vector<std::string> paths;
    for (int i = 0; i < 3; ++i) {
        paths.push_back(workspace.write(std::format("drive{}.step", i), test::driveStep()));
    }
    FakePainter painter;
    Recorder recorder;
    library::Service service(workspace.config, painter, recorder);
    service.want(wantAll(paths));
    service.waitIdle();
    for (const std::string& path : paths) {
        CAPTURE(path);
        const std::vector<scene::Facts>& seen = recorder.seen[path];
        REQUIRE(seen.size() >= 2);
        CHECK(seen.front().estimated);
        CHECK(seen.front().parts == 6);
        CHECK_FALSE(seen.back().estimated);
        CHECK(recorder.pictures.contains(path));
    }
    CHECK(service.stats().structures >= 1);
    CHECK(service.stats().imports == 3);
}
#endif
