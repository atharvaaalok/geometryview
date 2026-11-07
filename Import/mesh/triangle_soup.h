// The output of a mesh file parser: triangles with no connectivity.
#pragma once

#include "base/types.h"

#include <string>
#include <vector>

namespace geo::import {

struct TriangleSoup {
    std::vector<float> corners; // nine floats per triangle
    std::vector<std::string> warnings;

    usize triangleCount() const { return corners.size() / 9; }
};

} // namespace geo::import
