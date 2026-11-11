// Saves rendered pixels as a PNG.
#pragma once

#include "base/types.h"

#include <vector>

namespace geo::engine {

/// What a frame's alpha means: nothing, for a frame drawn on its own sheet, or coverage with the
/// colour premultiplied, for one drawn on a clear sheet.
enum class Alpha { Ignore, Premultiplied };

/// Writes BGRA pixels, as the renderer returns them, to `path`. Returns false on failure.
bool writePng(const char* path, const std::vector<u8>& bgra, u32 width, u32 height,
              Alpha alpha = Alpha::Ignore);

} // namespace geo::engine
