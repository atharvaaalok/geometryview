// The only code that calls OCCT. Everything here runs in a worker process, one request at a time.
#pragma once

#include "base/types.h"
#include "occt/worker_protocol.h"

#include <string>
#include <string_view>
#include <vector>

namespace geo::import::worker {

/// Sets up OCCT once per process.
void initializeOcct();

/// Meshes the faces of a STEP slice. Fills `reply` and appends the payload to `payload`.
void meshSlice(std::string_view text, const Request& request, Reply& reply,
               std::vector<u8>& payload);

/// Reads a whole STEP text with OCCT's full reader, as a fallback for files that cannot be sliced.
void meshWholeFile(std::string_view text, const Request& request, Reply& reply,
                   std::vector<u8>& payload);

/// Totals from reading a file the ordinary way, as ground truth for the slicing pipeline.
struct Truth {
    u32 faces = 0;
    u32 unmeshedFaces = 0;
    u64 triangles = 0;
    double area = 0.0;
    double signedVolume = 0.0;
    double min[3] = {0, 0, 0};
    double max[3] = {0, 0, 0};
    bool ok = false;
};

Truth measureFile(const char* path, double linear, double angular, double minSize);

} // namespace geo::import::worker
