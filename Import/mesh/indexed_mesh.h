// Turns unconnected triangles into an indexed mesh with faces and smooth normals.
#pragma once

#include "base/types.h"
#include "mesh/triangle_soup.h"

#include <vector>

namespace geo::import {

/// A mesh ready for packing. Vertices are split per smoothing group, so a crease stays sharp, and a
/// vertex belongs to exactly one face.
struct IndexedMesh {
    std::vector<float> positions;  // xyz per vertex
    std::vector<float> normals;    // unit xyz per vertex
    std::vector<u32> faceOfVertex; // one per vertex
    std::vector<u32> indices;      // three per triangle
    u32 faceCount = 0;
    bool closed = false; // every edge is shared by exactly two triangles
    /// Face ids are already in spatial order and must not change, as with STEP faces, whose ids
    /// have to survive refinement.
    bool faceOrderIsFixed = false;
    /// The coarsest grid step that keeps the geometry's tolerance, or zero to judge from edges.
    float maxGridStep = 0.0f;

    usize vertexCount() const { return faceOfVertex.size(); }
    usize triangleCount() const { return indices.size() / 3; }
};

struct RegionOptions {
    /// Triangles meeting at less than this angle share a smoothing group.
    float creaseDegrees = 30.0f;
    /// Upper bound on large pick faces. Smaller regions merge into neighbours, so a noisy scan gets
    /// a usable number of hoverable faces, while its creases remain sharp.
    u32 maxLargeFaces = 20000;
};

IndexedMesh buildIndexedMesh(const TriangleSoup& soup, const RegionOptions& options = {});

} // namespace geo::import
