// Runs the import helper and manages the cache.
#pragma once

#include <string>

namespace geo::ipc {

struct ImportPaths {
    std::string helper;
    std::string cacheDirectory;
};

struct ImportOutcome {
    bool ok = false;
    std::string blobPath;
    std::string message;
};

/// Returns the cache file for a source, or nothing if the source cannot be examined. The name is
/// derived from the path, identity, size and modification time, so an edited file gets a new entry,
/// and from the format version, so an old cache is never misread.
std::string cachePathFor(const std::string& sourcePath, const ImportPaths& paths,
                         std::string& error);

/// Imports `sourcePath` unless it is already cached. Blocks, so it runs off the engine thread.
///
/// The helper writes to a temporary file, which is renamed only after it succeeds. A reader therefore
/// never sees a partial file, and a mapping of an older version stays valid.
ImportOutcome importFile(const std::string& sourcePath, const ImportPaths& paths);

} // namespace geo::ipc
