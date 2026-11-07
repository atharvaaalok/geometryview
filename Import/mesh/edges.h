// Feature edges: the lines a drawing inks where faces meet, and where open surfaces end.
#pragma once

#include "base/types.h"
#include "format/gpu_records.h"
#include "mesh/grid.h"
#include "mesh/indexed_mesh.h"

#include <array>
#include <span>
#include <vector>

namespace geo::import {

/// How the packer stores a mesh, which edges are expressed in.
struct EdgeLayout {
    VertexGrid grid;
    simd_float3 boundsScale;           // 16-bit bounds grid to definition space
    std::span<const u32> packedOfFace; // the mesh's face to its packed face
};

/// Segments' ends are mesh vertices until `usePackedVertices` renames them.
struct FeatureEdges {
    std::vector<EdgeSegment> segments;
    std::vector<EdgeChunk> chunks;
};

/// Finds every edge where two faces meet, or where a surface has nothing on the other side.
///
/// Sides are matched by their packed grid positions, which is exactly what the GPU draws, so two
/// faces that share an edge in the model share it here even though they share no vertices. An edge
/// is a crease if the surface normals on its two sides differ by more than a few degrees, and
/// smooth otherwise. A smooth edge inside one face is a seam, such as where a cylinder's surface
/// closes on itself, and is dropped.
///
/// Segments are joined into chains, so dashes run on across segments, and chains are ordered in
/// space, so each chunk of segments is compact enough to cull.
///
/// This needs only the mesh and its grid, so it can run while meshlets are built.
FeatureEdges findFeatureEdges(const IndexedMesh& mesh, const EdgeLayout& layout);

/// Renames segments' ends from mesh vertices to packed ones. `packedVertex` gives, for each mesh
/// vertex, one packed vertex at its place.
void usePackedVertices(FeatureEdges& edges, std::span<const u32> packedVertex);

} // namespace geo::import
