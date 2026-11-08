#include "ipc/import_job.h"

#include "format/blob.h"

#include <cstdlib>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace geo;
using namespace geo::ipc;
namespace fs = std::filesystem;

namespace {

constexpr const char* kTriangle = "solid s\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendsolid s\n";

// Keeps what the import reported.
struct Collector final : ImportListener {
    std::vector<std::shared_ptr<scene::Segment>> segments;
    usize progressCalls = 0;

    void segmentReady(std::shared_ptr<scene::Segment> segment) override {
        segments.push_back(std::move(segment));
    }
    void progress(u32, u32) override { ++progressCalls; }
    void warning(const std::string&) override {}
};

ImportOutcome importFile(const std::string& source, const ImportPaths& paths) {
    Collector collector;
    return geo::ipc::importFile(source, paths, collector);
}

// A temporary directory holding a source file and a cache.
struct Workspace {
    fs::path root;
    ImportPaths paths;

    Workspace() {
        char pattern[] = "/tmp/geoimportXXXXXX";
        REQUIRE(mkdtemp(pattern) != nullptr);
        root = pattern;
        fs::create_directory(root / "cache");
        paths = {GEO_TEST_HELPER_PATH, (root / "cache").string()};
    }

    ~Workspace() { fs::remove_all(root); }

    std::string write(const char* name, const char* contents) const {
        const fs::path path = root / name;
        std::ofstream(path) << contents;
        return path.string();
    }

    usize cacheEntries() const {
        return usize(std::distance(fs::directory_iterator(root / "cache"), {}));
    }
};

} // namespace

TEST_CASE("importing through the helper produces a valid cache file") {
    Workspace workspace;
    const ImportOutcome outcome =
        importFile(workspace.write("part.stl", kTriangle), workspace.paths);
    REQUIRE_MESSAGE(outcome.ok, outcome.message);
    CHECK(fs::file_size(outcome.blobPath) > 0);
    CHECK(workspace.cacheEntries() == 1);
}

TEST_CASE("a miss and a hit deliver the same segments") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    Collector miss, hit;
    const ImportOutcome first = geo::ipc::importFile(source, workspace.paths, miss);
    const ImportOutcome second = geo::ipc::importFile(source, workspace.paths, hit);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK_FALSE(first.fromCache);
    CHECK(second.fromCache);
    REQUIRE(miss.segments.size() == 2);
    REQUIRE(hit.segments.size() == 2);
    for (usize i = 0; i < miss.segments.size(); ++i) {
        CHECK(miss.segments[i]->size() == hit.segments[i]->size());
    }
}

TEST_CASE("a damaged cache file is replaced") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    const ImportOutcome first = importFile(source, workspace.paths);
    REQUIRE(first.ok);
    {
        std::fstream file(first.blobPath, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(0);
        file << "broken";
    }
    const ImportOutcome second = importFile(source, workspace.paths);
    REQUIRE(second.ok);
    CHECK_FALSE(second.fromCache);
}

TEST_CASE("a second import of the same file reuses the cache") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    const ImportOutcome first = importFile(source, workspace.paths);
    REQUIRE(first.ok);
    struct stat written{};
    REQUIRE(stat(first.blobPath.c_str(), &written) == 0);
    const ImportOutcome second = importFile(source, workspace.paths);
    REQUIRE(second.ok);
    CHECK(second.fromCache);
    CHECK(second.blobPath == first.blobPath);
    // The same file, not rewritten; a hit only touches it, to record the use for eviction.
    struct stat reused{};
    REQUIRE(stat(second.blobPath.c_str(), &reused) == 0);
    CHECK(reused.st_ino == written.st_ino);
    CHECK(reused.st_size == written.st_size);
}

TEST_CASE("editing a file gives it a new cache entry") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    const ImportOutcome first = importFile(source, workspace.paths);
    workspace.write("part.stl", "solid s\nvertex 0 0 0\nvertex 2 0 0\nvertex 0 2 0\nendsolid s\n");
    const ImportOutcome second = importFile(source, workspace.paths);
    REQUIRE(second.ok);
    CHECK(second.blobPath != first.blobPath);
}

TEST_CASE("a bad file reports the helper's message and leaves no cache behind") {
    Workspace workspace;
    const ImportOutcome outcome =
        importFile(workspace.write("bad.stl", "not an stl"), workspace.paths);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message == "This does not look like an STL file.");
    CHECK(workspace.cacheEntries() == 0);
}

TEST_CASE("a missing file reports that it could not be found") {
    Workspace workspace;
    const ImportOutcome outcome =
        importFile((workspace.root / "nope.stl").string(), workspace.paths);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message == "This file could not be found.");
}

TEST_CASE("a crashing helper is reported and leaves no cache behind") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    const fs::path crasher = workspace.root / "crash.sh";
    std::ofstream(crasher) << "#!/bin/sh\nkill -SEGV $$\n";
    fs::permissions(crasher, fs::perms::owner_all);
    workspace.paths.helper = crasher.string();

    const ImportOutcome outcome = importFile(source, workspace.paths);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message == "The import helper stopped unexpectedly. This file may be damaged.");
    CHECK(workspace.cacheEntries() == 0);
}

TEST_CASE("a helper that cannot start is reported") {
    Workspace workspace;
    const std::string source = workspace.write("part.stl", kTriangle);
    workspace.paths.helper = (workspace.root / "missing-helper").string();
    const ImportOutcome outcome = importFile(source, workspace.paths);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message.starts_with("The import helper could not start"));
    CHECK(workspace.cacheEntries() == 0);
}
