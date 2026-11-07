// Reads binary and ASCII STL.
#pragma once

#include "base/types.h"
#include "mesh/triangle_soup.h"

#include <span>
#include <string>

namespace geo::import {

/// Parses an STL file held in memory. Returns false and sets `error` if it is not a usable STL.
/// Recoverable problems, such as dropped non-finite triangles, become warnings on `soup`.
bool readStl(std::span<const u8> file, TriangleSoup& soup, std::string& error);

} // namespace geo::import
