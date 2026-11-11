// The gallery's scan: every STEP and STL file under a folder, in batches, bounded.
#include "library/scan.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace geo;
using library::ScanEntry;
namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path root;
    Tree() {
        char pattern[] = "/tmp/geoscanXXXXXX";
        REQUIRE(mkdtemp(pattern) != nullptr);
        root = pattern;
    }
    ~Tree() { fs::remove_all(root); }
    void file(const std::string& path, usize bytes = 10) const {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << std::string(bytes, 'x');
    }
    void folder(const std::string& path) const { fs::create_directories(root / path); }
};

std::vector<ScanEntry> scanAll(const Tree& tree, library::ScanResult& result,
                               const library::ScanLimits& limits = {}) {
    std::vector<ScanEntry> entries;
    result = library::scan(
        tree.root.string(),
        [&](std::span<const ScanEntry> batch) {
            entries.insert(entries.end(), batch.begin(), batch.end());
            return true;
        },
        limits);
    return entries;
}

std::map<std::string, ScanEntry> byPath(const std::vector<ScanEntry>& entries) {
    std::map<std::string, ScanEntry> map;
    for (const ScanEntry& entry : entries) {
        map[entry.path] = entry;
    }
    return map;
}

} // namespace

TEST_CASE("a scan lists model files and folders, each folder after what is under it") {
    Tree tree;
    tree.file("gear.STEP", 123);
    tree.file("notes.txt");
    tree.file("motors/nema17.stp");
    tree.file("motors/small/n8.stl");
    tree.file("motors/readme.md");
    tree.file(".hidden.step");
    tree.file(".git/objects/x.step");
    tree.folder("empty");
    fs::create_symlink(tree.root / "gear.STEP", tree.root / "link.step");

    library::ScanResult result;
    const std::vector<ScanEntry> entries = scanAll(tree, result);
    CHECK(result.error.empty());
    CHECK(result.files == 3);
    CHECK(result.folders == 3);
    CHECK_FALSE(result.truncated);
    const auto map = byPath(entries);
    REQUIRE(map.size() == 6);
    CHECK(map.at("gear.STEP").bytes == 123);
    CHECK(map.at("gear.STEP").modified > 1.7e9);
    CHECK(map.at("motors").folder);
    CHECK(map.at("motors").files == 2);
    CHECK(map.at("motors/small").files == 1);
    CHECK(map.at("empty").files == 0);
    CHECK(map.contains("motors/small/n8.stl"));
    // A folder after its contents.
    const auto at = [&](const std::string& path) {
        return std::find_if(entries.begin(), entries.end(),
                            [&](const ScanEntry& e) { return e.path == path; }) -
               entries.begin();
    };
    CHECK(at("motors/small/n8.stl") < at("motors/small"));
    CHECK(at("motors/small") < at("motors"));
}

TEST_CASE("a scan stops at its limits, and says so") {
    Tree tree;
    for (int i = 0; i < 30; ++i) {
        tree.file("parts/p" + std::to_string(i) + ".stl");
    }
    tree.file("a/b/c/d/deep.step");

    library::ScanResult result;
    library::ScanLimits limits;
    limits.maxFiles = 10;
    std::vector<ScanEntry> entries = scanAll(tree, result, limits);
    CHECK(result.files == 10);
    CHECK(result.truncated);

    limits = {};
    limits.maxDepth = 3;
    entries = scanAll(tree, result, limits);
    CHECK(result.files == 30);
    CHECK(result.truncated);
    CHECK_FALSE(byPath(entries).contains("a/b/c/d/deep.step"));
}

TEST_CASE("a scan hands over batches, and stops when asked") {
    Tree tree;
    for (int i = 0; i < 100; ++i) {
        tree.file("p" + std::to_string(i) + ".step");
    }
    library::ScanLimits limits;
    limits.batch = 16;
    std::vector<usize> sizes;
    library::ScanResult result = library::scan(
        tree.root.string(),
        [&](std::span<const ScanEntry> batch) {
            sizes.push_back(batch.size());
            return sizes.size() < 3;
        },
        limits);
    CHECK(result.stopped);
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == 16);
}

TEST_CASE("a scan of what is not a folder says why") {
    Tree tree;
    tree.file("gear.step");
    library::ScanResult result =
        library::scan((tree.root / "missing").string(), [](auto) { return true; });
    CHECK(result.error == "This folder could not be found.");
    result = library::scan((tree.root / "gear.step").string(), [](auto) { return true; });
    CHECK(result.error == "This is not a folder.");
}

TEST_CASE("a listing reads back what it writes") {
    library::Listing listing;
    listing.root = "/Users/someone/Parts";
    listing.scanned = 1.8e9 + 0.25;
    listing.truncated = true;
    listing.entries = {{"motors/n17.step", 1234, 1.7e9 + 0.5, false, 0},
                       {"motors", 0, 0.0, true, 1}};
    json::Value value;
    std::string error;
    REQUIRE(json::parse(listing.toJson().write(), value, error));
    library::Listing back;
    REQUIRE(library::Listing::fromJson(value, back));
    CHECK(back.root == listing.root);
    CHECK(back.truncated);
    CHECK(back.scanned == listing.scanned);
    CHECK(back.entries == listing.entries);
    CHECK(library::listingPath("/c", "/a") != library::listingPath("/c", "/b"));
    CHECK(library::listingPath("/c", "/a").ends_with(".json"));
}
