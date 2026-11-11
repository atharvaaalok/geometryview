// The gallery's library through its C interface: a folder scanned, its files imported and drawn
// by the engine's thumbnailer, and its listing kept.
#include "library.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(GEO_TEST_SHADER_LIBRARY)

namespace fs = std::filesystem;

namespace {

struct Seen {
    std::mutex mutex;
    std::map<std::string, GeoFacts> facts;
    std::map<std::string, std::string> names;
    std::map<std::string, GeoPicture> pictures;
    std::map<std::string, std::string> pngs;
    std::vector<std::string> failures;
};

void receive(void* context, const GeoLibraryEvent* event) {
    Seen& seen = *static_cast<Seen*>(context);
    std::lock_guard lock(seen.mutex);
    switch (event->kind) {
    case GeoLibraryFacts:
        seen.facts[event->path] = event->facts;
        seen.names[event->path] = event->facts.name;
        break;
    case GeoLibraryPicture:
        seen.pictures[event->path] = event->picture;
        seen.pngs[event->path] = event->picture.png_path;
        break;
    case GeoLibraryFailed:
        seen.failures.push_back(event->path);
        break;
    case GeoLibraryProgress:
        break;
    }
}

struct Entries {
    std::vector<std::string> paths;
    std::vector<bool> folders;
};

bool collect(void* context, const GeoScanEntry* entries, uint32_t count) {
    Entries& out = *static_cast<Entries*>(context);
    for (uint32_t i = 0; i < count; ++i) {
        out.paths.push_back(entries[i].path);
        out.folders.push_back(entries[i].folder);
    }
    return true;
}

std::string triangleStl(double size) {
    return std::format("solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex {} 0 0\n"
                       "vertex 0 {} {}\nendloop\nendfacet\nendsolid t\n",
                       size, size, size);
}

} // namespace

TEST_CASE("a folder scanned, wanted and drawn through the library's C interface") {
    char pattern[] = "/tmp/geolibraryXXXXXX";
    REQUIRE(mkdtemp(pattern) != nullptr);
    const fs::path root = pattern;
    fs::create_directories(root / "parts" / "small");
    fs::create_directories(root / "cache");
    for (int i = 0; i < 3; ++i) {
        std::ofstream(root / "parts" / (i == 2 ? "small/c.stl" : std::format("p{}.stl", i)))
            << triangleStl(10.0 + i);
    }

    Seen seen;
    GeoLibraryDescriptor descriptor{};
    descriptor.shader_library_path = GEO_TEST_SHADER_LIBRARY;
    descriptor.import_helper_path = GEO_TEST_HELPER_PATH;
    const std::string cache = (root / "cache").string();
    descriptor.cache_directory = cache.c_str();
    descriptor.callback = receive;
    descriptor.context = &seen;
    GeoLibrary* library = geo_library_create(&descriptor);
    REQUIRE(library != nullptr);

    Entries entries;
    GeoScanSummary summary{};
    char error[256] = {};
    const std::string folder = (root / "parts").string();
    REQUIRE(geo_library_scan(library, folder.c_str(), collect, &entries, &summary, error,
                             sizeof(error)));
    CHECK(summary.files == 3);
    CHECK(summary.folders == 1);

    GeoPictureSpec spec{GeoStyleIllustration, GeoColorParts, 128, 2.0f, 0.5f};
    geo_library_set_picture(library, &spec);
    std::vector<std::string> paths;
    std::vector<GeoLibraryWant> wants;
    for (size_t i = 0; i < entries.paths.size(); ++i) {
        if (!entries.folders[i]) {
            paths.push_back((root / "parts" / entries.paths[i]).string());
        }
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        wants.push_back({paths[i].c_str(), GeoBandVisible, double(i), 0, 0.0, 2});
    }
    geo_library_want(library, wants.data(), uint32_t(wants.size()));
    geo_library_wait_idle(library);

    CHECK(seen.failures.empty());
    for (const std::string& path : paths) {
        CAPTURE(path);
        REQUIRE(seen.facts.contains(path));
        CHECK_FALSE(seen.facts[path].estimated);
        CHECK(seen.facts[path].parts == 1);
        REQUIRE(seen.pictures.contains(path));
        const GeoPicture& picture = seen.pictures[path];
        CHECK(picture.width == 128);
        CHECK(picture.part_rect[2] > 0);
        CHECK(picture.first_color == 2); // one part: its folder's colour
        CHECK(fs::file_size(seen.pngs[path]) > 100);
    }
    const GeoLibraryStats stats = geo_library_stats(library);
    CHECK(stats.imports == 3);
    CHECK(stats.pictures == 3);

    // The listing is kept for next time.
    Entries kept;
    REQUIRE(geo_library_kept_listing(library, folder.c_str(), collect, &kept));
    CHECK(kept.paths == entries.paths);
    geo_library_destroy(library);
    fs::remove_all(root);
}

#endif
