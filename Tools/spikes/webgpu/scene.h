// The renderer spike's view of a cache file: the tables its shaders read, and where each piece of
// the file's geometry goes in the GPU's streams. Nothing is copied on the CPU: pieces point into
// the file's bytes, and the renderer uploads them straight to the GPU.
#pragma once

#include "base/types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace geo::spike {

// 80 bytes, as the shaders' Definition. Bases index the streams in elements.
struct GpuDefinition {
    u32 meshletBase, vertexBase, highBitsBase, triangleBase;
    float boundsMin[3];
    float boundsStep;
    float decodeScale[3];
    u32 flags;
    float boundsScale[3];
    u32 vertexCount;
    u32 edgeChunkBase, edgeSegmentBase, faceCount, pad0;
};
static_assert(sizeof(GpuDefinition) == 80);

// 64 bytes: a 3x4 transform by rows, the definition and the first pick id.
struct GpuOccurrence {
    float rows[3][4];
    u32 definition, idBase, pad0, pad1;
};
static_assert(sizeof(GpuOccurrence) == 64);

enum Stream : u32 {
    kStreamMeshlets,
    kStreamVertices,
    kStreamHighBits,
    kStreamTriangles,
    kStreamEdgeChunks,
    kStreamEdgeSegments,
    kStreams
};

// A run of the file's bytes, and where it goes in a stream.
struct Piece {
    Stream stream;
    u64 offset;
    const u8* data;
    u64 size;
};

struct SceneData {
    std::vector<GpuDefinition> definitions;
    std::vector<GpuOccurrence> occurrences;
    // Every meshlet of every occurrence, as (occurrence, meshlet within its definition): what the
    // cull pass tests, one thread each.
    std::vector<std::array<u32, 2>> instances;
    // Every edge chunk of every occurrence, as (occurrence, chunk within its definition).
    std::vector<std::array<u32, 2>> edgeInstances;
    std::array<u64, kStreams> streamBytes{};
    std::vector<Piece> pieces;
    // 16-triangle clusters the cull pass can emit at most, for closed and for open surfaces.
    u64 closedClusters = 0, openClusters = 0;
    u64 closedInstances = 0; // meshlet instances of closed surfaces
    u64 triangles = 0; // counting every occurrence
    std::array<float, 3> boundsMin{}, boundsMax{};
};

/// Builds the tables for a finished cache file. `copies` repeats the whole model side by side
/// along x, to make larger scenes from one file.
bool buildScene(std::span<const u8> file, u32 copies, SceneData& scene, std::string& error);

} // namespace geo::spike
