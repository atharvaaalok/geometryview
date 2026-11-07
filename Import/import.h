// Turns a source file into a cache file. Shared by the helper process and gcli.
#pragma once

#include "base/types.h"
#include "cache/blob_writer.h"
#include "step/step_import.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace geo::import {

struct StageTimes {
    double parse = 0, regions = 0, pack = 0, write = 0;
};

struct ImportResult {
    usize triangles = 0;
    u32 faces = 0;
    usize meshlets = 0;
    bool wide = false;
    StageTimes times;
    std::vector<std::string> warnings;
};

/// Hashes file contents, for keying the cache.
std::array<u8, 16> hashContents(std::span<const u8> file);

/// Imports a mesh file held in memory and writes a blob to `outputFd`. `listener` may be null.
bool importMesh(std::span<const u8> file, int outputFd, SegmentListener* listener,
                ImportResult& result, std::string& error);

} // namespace geo::import
