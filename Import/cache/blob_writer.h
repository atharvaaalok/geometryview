// Writes cache files.
#pragma once

#include "base/types.h"
#include "mesh/packer.h"

#include <array>
#include <string>

namespace geo::import {

/// Writes one definition with one identity occurrence to the open file `fd`. Returns false and sets
/// `error` on failure. Callers write to a temporary file and rename it, so a reader never sees a
/// partial file and an existing mapping stays valid.
bool writeBlob(int fd, const PackedDefinition& definition,
               const std::array<u8, 16>& sourceHash, std::string& error);

} // namespace geo::import
