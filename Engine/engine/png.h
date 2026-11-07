// Saves rendered pixels as a PNG.
#pragma once

#include "base/types.h"

#include <vector>

namespace geo::engine {

/// Writes BGRA pixels, as the renderer returns them, to `path`. Returns false on failure.
bool writePng(const char* path, const std::vector<u8>& bgra, u32 width, u32 height);

} // namespace geo::engine
