// Imports a STEP file: index it, find its distinct bodies, and send them through OCCT workers in
// small slices, packing each definition as its last slice comes back.
#pragma once

#include "base/types.h"
#include "cache/blob_writer.h"

#include <span>
#include <string>
#include <vector>

namespace geo::import {

struct StepOptions {
    std::string workerPath;
    int sourceFd = -1;    // the same file, for the whole-file fallback
    usize maxWorkers = 0; // zero uses one per performance core
    u32 level = 0;        // tessellation level; each is twice as fine as the one before
    /// Definitions to import, for refinement. Empty imports everything, with the model.
    std::vector<u32> definitions;
};

struct StepStats {
    usize records = 0;
    usize bodies = 0;
    usize definitions = 0;
    usize occurrences = 0;
    usize chunks = 0;
    usize faces = 0;
    usize triangles = 0;
    usize meshlets = 0;
    u32 failedFaces = 0;
    u32 unmeshedFaces = 0;
    u32 unmappedFaces = 0;
    u32 crashedWorkers = 0;
    usize workers = 0;
    double linearDeflection = 0.0;
    double angularDeflection = 0.0;
    double minSize = 0.0;
    bool wholeFile = false;
    double tokenizeSeconds = 0.0;
    double structureSeconds = 0.0;
    double definitionSeconds = 0.0;
    double workerSeconds = 0.0; // wall time until the last slice came back
    double readSeconds = 0.0;   // sums across workers
    double transferSeconds = 0.0;
    double meshSeconds = 0.0;
    double packSeconds = 0.0;
    std::vector<std::string> warnings;
};

/// True if `file` starts like a STEP file.
bool looksLikeStep(std::span<const u8> file);

bool importStep(std::span<const u8> file, int outputFd, SegmentListener* listener,
                const StepOptions& options, StepStats& stats, std::string& error);

} // namespace geo::import
