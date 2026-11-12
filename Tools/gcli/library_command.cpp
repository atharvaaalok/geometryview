// gcli library: runs the gallery's library over a folder, headless, and reports how it went, for
// choosing how many imports run at once and with how many workers each.
#include "library_command.h"

#include "library.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace geo::gcli {
namespace {

struct Tally {
    std::mutex mutex;
    uint32_t facts = 0, pictures = 0, failures = 0;
    std::vector<std::string> failed;
};

void receive(void* context, const GeoLibraryEvent* event) {
    Tally& tally = *static_cast<Tally*>(context);
    std::lock_guard lock(tally.mutex);
    switch (event->kind) {
    case GeoLibraryFacts:
        tally.facts += event->facts.estimated ? 0 : 1;
        break;
    case GeoLibraryPicture:
        ++tally.pictures;
        break;
    case GeoLibraryFailed:
        ++tally.failures;
        tally.failed.push_back(std::string(event->path) + ": " + event->message);
        break;
    case GeoLibraryProgress:
    case GeoLibraryTurnFrame: // no turntables asked for here
        break;
    }
}

struct Found {
    std::vector<std::string> paths;
    std::vector<uint64_t> bytes;
    std::vector<double> modified;
    std::string root;
    uint32_t limit = 0;
};

bool collect(void* context, const GeoScanEntry* entries, uint32_t count) {
    Found& found = *static_cast<Found*>(context);
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].folder) {
            continue;
        }
        if (found.limit != 0 && found.paths.size() == found.limit) {
            return false;
        }
        found.paths.push_back(found.root + "/" + entries[i].path);
        found.bytes.push_back(entries[i].bytes);
        found.modified.push_back(entries[i].modified);
    }
    return true;
}

} // namespace

int libraryCommand(int argc, const char** argv) {
    // gcli library <metallib> <helper> <cache> <folder> [--slots S] [--workers W] [--style N]
    //              [--pixels P] [--limit N] [--prefetch]
    if (argc < 6) {
        std::println(stderr, "usage: gcli library <metallib> <helper> <cache> <folder> [--slots S] "
                             "[--workers W] [--style N] [--pixels P] [--limit N] [--prefetch]");
        return 2;
    }
    GeoLibraryDescriptor descriptor{};
    descriptor.shader_library_path = argv[2];
    descriptor.import_helper_path = argv[3];
    descriptor.cache_directory = argv[4];
    Found found;
    found.root = argv[5];
    GeoPictureSpec spec{GeoStyleIllustration, GeoColorParts, 256, 2.0f, 0.5f};
    bool prefetch = false;
    for (int i = 6; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : "0";
        if (flag == "--slots") {
            descriptor.slots = uint32_t(std::atoi(value));
            ++i;
        } else if (flag == "--workers") {
            descriptor.workers = uint32_t(std::atoi(value));
            ++i;
        } else if (flag == "--style") {
            spec.style = GeoStyle(std::atoi(value));
            ++i;
        } else if (flag == "--pixels") {
            spec.pixels = uint32_t(std::atoi(value));
            ++i;
        } else if (flag == "--limit") {
            found.limit = uint32_t(std::atoi(value));
            ++i;
        } else if (flag == "--prefetch") {
            prefetch = true;
        } else {
            std::println(stderr, "unknown option {}", flag);
            return 2;
        }
    }

    Tally tally;
    descriptor.callback = receive;
    descriptor.context = &tally;
    GeoLibrary* library = geo_library_create(&descriptor);
    if (library == nullptr) {
        std::println(stderr, "could not start the library on {}", descriptor.cache_directory);
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    GeoScanSummary summary{};
    char error[256] = {};
    if (!geo_library_scan(library, found.root.c_str(), collect, &found, &summary, error,
                          sizeof(error))) {
        std::println(stderr, "{}", error);
        geo_library_destroy(library);
        return 1;
    }
    const double scanned = std::chrono::duration<double>(Clock::now() - start).count();

    geo_library_set_picture(library, &spec);
    geo_library_set_prefetch(library, true);
    // Everything in view, in order, as a gallery scrolled through the folder would want it; or
    // everything prefetched, as an idle gallery would.
    std::vector<GeoLibraryWant> wants;
    for (size_t i = 0; i < found.paths.size(); ++i) {
        wants.push_back({found.paths[i].c_str(), prefetch ? GeoBandPrefetch : GeoBandVisible,
                         double(i), found.bytes[i], found.modified[i], -1});
    }
    geo_library_want(library, wants.data(), uint32_t(wants.size()));
    geo_library_wait_idle(library);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const GeoLibraryStats stats = geo_library_stats(library);
    geo_library_destroy(library);

    rusage children{};
    getrusage(RUSAGE_CHILDREN, &children);
    std::println("{{\"files\": {}, \"scan_seconds\": {:.3f}, \"seconds\": {:.3f}, "
                 "\"files_per_minute\": {:.1f}, \"facts\": {}, \"pictures\": {}, "
                 "\"failures\": {}, \"helpers\": {}, \"imports\": {}, \"structures\": {}, "
                 "\"lookups\": {}, \"largest_child_mb\": {:.1f}, \"slots\": {}, \"workers\": {}}}",
                 found.paths.size(), scanned, seconds, double(found.paths.size()) / seconds * 60.0,
                 tally.facts, tally.pictures, tally.failures, stats.helpers, stats.imports,
                 stats.structures, stats.lookups, double(children.ru_maxrss) / (1 << 20),
                 descriptor.slots, descriptor.workers);
    for (const std::string& failure : tally.failed) {
        std::println(stderr, "failed: {}", failure);
    }
    return 0;
}

} // namespace geo::gcli
