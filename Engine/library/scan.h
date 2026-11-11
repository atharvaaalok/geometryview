// Lists a folder for the gallery: every STEP and STL file under it, and its folders, in batches as
// they are found, so a large tree shows its first files at once. The walk is bounded, since a
// gallery can be pointed at a whole disk: it never follows links, skips hidden files and folders,
// and stops at a depth and a number of files.
#pragma once

#include "base/json.h"
#include "base/types.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geo::library {

struct ScanEntry {
    std::string path;      // relative to the root, with '/' between names
    u64 bytes = 0;         // a file's size
    double modified = 0.0; // seconds since 1970
    bool folder = false;
    u32 files = 0; // a folder's STEP and STL files, at any depth

    bool operator==(const ScanEntry&) const = default;
};

struct ScanLimits {
    u32 maxDepth = 32;
    u32 maxFiles = 20000;
    usize batch = 256;
    double batchSeconds = 0.05;
};

struct ScanResult {
    u32 files = 0;
    u32 folders = 0;
    bool truncated = false; // a limit stopped the walk
    bool stopped = false;   // the caller stopped it
    std::string error;      // the root could not be read
};

/// Whether a file name is one the gallery shows: .step, .stp or .stl, in any case.
bool isModelFile(std::string_view name);

/// Walks `root`, handing over entries in batches of at most `limits.batch`, and at least every
/// `limits.batchSeconds` while any are found. A folder comes after everything under it, so it
/// knows its count of files. `batch` returns false to stop the walk.
ScanResult scan(const std::string& root,
                const std::function<bool(std::span<const ScanEntry>)>& batch,
                const ScanLimits& limits = {});

/// A folder's last listing, kept so a gallery shows it at once when it opens again, then checks it.
struct Listing {
    std::string root;
    double scanned = 0.0; // seconds since 1970
    bool truncated = false;
    std::vector<ScanEntry> entries;

    json::Value toJson() const;
    static bool fromJson(const json::Value& value, Listing& listing);
};

/// Where a root's listing is kept in `directory`: named by a hash of its path.
std::string listingPath(const std::string& directory, const std::string& root);

} // namespace geo::library
