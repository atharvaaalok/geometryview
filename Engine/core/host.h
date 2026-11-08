// What the engine's core needs from the platform it runs on: time, frames, imports, and a way to
// tell the app what changed. The Mac engine provides it with a thread, a display link and import
// processes; the universal viewer will provide it with the browser's frame loop and Web Workers.
#pragma once

#include "base/types.h"
#include "engine.h"
#include "model/visibility.h"
#include "scene/mapped_segment.h"

#include <memory>
#include <string>
#include <vector>

namespace geo::engine {

/// How an import or refinement ended.
struct ImportResult {
    bool ok = false;
    bool fromCache = false;
    std::string message;  // why it failed
    std::string blobPath; // its cache file, if it has one
};

/// What the app reads of the selection, and the counts it shows.
struct Summary {
    GeoSelectionSummary selection{0, 0, 0, GEO_NO_NODE, GeoSelectFaces};
    u32 selectedOccurrences = 0;
    u32 hiddenOccurrences = 0;
};

class Host {
public:
    virtual ~Host() = default;

    /// Monotonic seconds.
    virtual double now() const = 0;
    /// Whether the user asked for less motion, which makes camera flights and fades instant.
    virtual bool reduceMotion() const = 0;

    /// Starts or stops the frame clock, which calls `Core::frame` while running.
    virtual void runFrames(bool running) = 0;
    /// Calls `Core::refinementDue` after `delaySeconds`, replacing any earlier request.
    virtual void scheduleRefinement(double delaySeconds) = 0;

    /// Imports a file for the document `token`. Results arrive through the core's `addSegment`,
    /// `importProgress` and `importFinished`, on the core's own thread.
    virtual void startImport(u64 token, const std::string& path) = 0;
    /// Imports `definitions` of an open file again at `level`, finer, ending in
    /// `refinementFinished`. One runs at a time.
    virtual void startRefinement(u64 token, const std::string& path, u32 level,
                                 std::vector<u32> definitions) = 0;
    /// Abandons a document's import, or the refinement, whose results are then ignored anyway.
    virtual void cancelImport(u64 token) = 0;
    virtual void cancelRefinement() = 0;
    /// Trims the cache to its budget, sparing the files open now.
    virtual void evictCache(std::vector<std::string> keep) = 0;

    /// Tells the app something happened.
    virtual void emit(const GeoEvent& event) = 0;
    /// Hands the app a document's tree, or takes them all back when everything closes.
    virtual void publishModel(u32 document, std::shared_ptr<scene::MappedSegment> model) = 0;
    virtual void clearModels() = 0;
    /// Each occurrence's swatch colour in global order, and each document's first occurrence.
    virtual void publishSwatches(std::vector<u32> swatches, std::vector<u32> bases) = 0;
    /// Each tree node's state, per document.
    virtual void publishNodeStates(std::vector<std::vector<model::NodeVisibility>> states) = 0;
    virtual void publishSummary(const Summary& summary) = 0;

    /// Notes memory use at a moment worth knowing, when tracing. Optional.
    virtual void traceMemory(const char* label) { (void)label; }
};

} // namespace geo::engine
