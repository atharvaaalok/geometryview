// Packs an indexed mesh into the compact, GPU-ready records stored in cache files.
#pragma once

#include "base/types.h"
#include "format/gpu_records.h"
#include "mesh/indexed_mesh.h"

#include <array>
#include <vector>

namespace geo::import {

struct PackedDefinition {
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    std::array<float, 3> decodeScale{}; // grid units to definition space
    std::array<float, 3> boundsScale{}; // 16-bit bounds grid to definition space
    u32 flags = 0;
    u32 faceCount = 0;
    std::vector<PackedVertex> vertices;
    std::vector<u32> highBits; // one per vertex in wide definitions, otherwise empty
    std::vector<u32> triangles;
    std::vector<Meshlet> meshlets;
    /// Packed face id to the caller's face id. Packing renumbers faces so that faces close in space
    /// have close ids, which is what lets a meshlet use a 16-bit local face.
    std::vector<u32> originalFace;
};

PackedDefinition packMesh(const IndexedMesh& mesh);

/// Decodes a vertex's position back to definition space, for tests and CPU-side consumers.
std::array<float, 3> decodePosition(const PackedDefinition& definition, usize vertex);

} // namespace geo::import
