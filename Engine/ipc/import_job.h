// Runs the import helper and manages the cache.
#pragma once

#include "base/types.h"
#include "scene/segment.h"

#include <memory>
#include <span>
#include <string>

namespace geo::ipc {

struct ImportPaths {
    std::string helper;
    std::string cacheDirectory;
};

struct ImportOutcome {
    bool ok = false;
    bool fromCache = false;
    std::string blobPath;
    std::string message;
};

/// Receives results as they become available, on the importing thread.
class ImportListener {
public:
    virtual ~ImportListener() = default;
    virtual void segmentReady(std::shared_ptr<scene::Segment> segment) = 0;
    virtual void progress(u32 done, u32 total) = 0;
    virtual void warning(const std::string& text) = 0;
};

/// Returns the cache file for a source, or nothing if the source cannot be examined. The name is
/// derived from the path, identity, size and modification time, so an edited file gets a new entry,
/// and from the format version, so an old cache is never misread.
std::string cachePathFor(const std::string& sourcePath, const ImportPaths& paths,
                         std::string& error);

/// Allows an import in progress to be abandoned from another thread.
class ImportCancel {
public:
    ImportCancel();

    /// Stops the helper, if there is one.
    void cancel();
    void setHelper(int pid);
    bool cancelled() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

/// How an import runs, beyond what it reads.
struct ImportOptions {
    /// OCCT workers at most; zero uses one per performance core.
    u32 workers = 0;
};

/// Imports `sourcePath`, or reads it from the cache. Either way the listener receives the same
/// segments, so a hit and a miss share one path. Blocks, so it runs off the engine thread.
///
/// The helper writes to a temporary file, which is renamed only after it succeeds. Reading the
/// final name therefore never sees a partial file, and mappings of the temporary one stay valid.
ImportOutcome importFile(const std::string& sourcePath, const ImportPaths& paths,
                         ImportListener& listener, ImportCancel* cancel = nullptr,
                         const ImportOptions& options = {});

/// Whether `importFile` would find `sourcePath` in the cache, without reading it.
bool isCached(const std::string& sourcePath, const ImportPaths& paths);

/// Reads only a STEP file's structure: the listener receives its model, with each body's box
/// estimated, and no geometry. Nothing is cached; the result is a moment's look, for a gallery to
/// lay the file out while it waits its turn to import. Fails for a file that only a whole read can
/// open, and for a mesh, which has no structure to read apart from its geometry.
ImportOutcome readStructure(const std::string& sourcePath, const ImportPaths& paths,
                            ImportListener& listener, ImportCancel* cancel = nullptr);

/// Imports some definitions again at a finer level, as segments that replace their coarser
/// geometry. Each result is cached beside the file's own, and `importFile` brings it back.
ImportOutcome refineFile(const std::string& sourcePath, const ImportPaths& paths, u32 level,
                         std::span<const u32> definitions, ImportListener& listener,
                         ImportCancel* cancel = nullptr);

} // namespace geo::ipc
