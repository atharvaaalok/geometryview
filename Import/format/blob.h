// The on-disk cache format. A cache file is a header followed by segments. Each segment stands
// alone, so it can be mapped and shown as soon as it is written, which is how a file appears while
// it imports. Reading a finished file takes the same path: the header lists its segments.
//
// A segment is mapped read-only and wrapped directly in a Metal buffer, so geometry exists once in
// memory and is backed by a clean file. Segments start and end on 16 KB boundaries, the page size
// on Apple silicon. Everything is little-endian.
#pragma once

#include "base/types.h"
#include "format/gpu_records.h"

#include <array>
#include <span>
#include <string>

namespace geo::format {

inline constexpr std::array<char, 8> kMagic = {'G', 'E', 'O', 'B', 'L', 'O', 'B', '\0'};
inline constexpr std::array<char, 8> kSegmentMagic = {'G', 'E', 'O', 'S', 'E', 'G', 'M', '\0'};
inline constexpr u32 kVersion = 4;
inline constexpr usize kAlignment = 16384;
inline constexpr usize kMaxSegments = 512;
inline constexpr usize kMaxSections = 14;
inline constexpr usize kSectionAlignment = 16;
inline constexpr u32 kNoParent = 0xFFFFFFFF;

enum class SectionKind : u32 {
    // The model: what the file contains. A file has one model, usually ahead of its geometry.
    DefinitionInfos = 1,
    Occurrences = 2,
    Nodes = 3,
    Strings = 4,
    // Geometry for some of the model's definitions.
    Definitions = 5,
    Meshlets = 6,
    Vertices = 7,
    HighBits = 8,
    Triangles = 9,
    Faces = 10,
    ModelInfo = 11,
    EdgeSegments = 12,
    EdgeChunks = 13,
};

struct SegmentEntry {
    u64 offset;
    u64 size;
};
static_assert(sizeof(SegmentEntry) == 16);

struct Header {
    std::array<char, 8> magic;
    u32 version;
    u32 segmentCount;
    u64 fileSize;
    std::array<u8, 16> sourceHash;
    std::array<SegmentEntry, kMaxSegments> segments;
};
static_assert(sizeof(Header) <= kAlignment);

/// Offsets are from the start of the segment.
struct SectionEntry {
    u32 kind;
    u32 stride;
    u64 offset;
    u64 size;
};
static_assert(sizeof(SectionEntry) == 24);

struct SegmentHeader {
    std::array<char, 8> magic;
    u32 version;
    u32 sectionCount;
    u64 size;
    std::array<SectionEntry, kMaxSections> sections;
};

/// Facts about the whole model. Zero tolerances mean the geometry cannot be refined, as for a mesh.
struct ModelInfo {
    double linearDeflection; // of level 0, in model units
    double angularDeflection;
    u32 maxLevel;
    u32 reserved[3];
};
static_assert(sizeof(ModelInfo) == 32);

/// What the model knows about a definition before its geometry exists. The face count fixes pick
/// ids up front, so they stay valid as geometry arrives in any order.
struct DefinitionInfo {
    std::array<float, 3> boundsMin; // estimated until geometry arrives
    std::array<float, 3> boundsMax;
    u32 faceCount;
    u32 flags;
};
static_assert(sizeof(DefinitionInfo) == 32);

struct OccurrenceRecord {
    std::array<float, 12> transform; // three columns of a 3x4 matrix, then translation
    u32 definition;
    u32 node;
    u32 color; // RGBA8, or 0 for none
    u32 reserved;
};
static_assert(sizeof(OccurrenceRecord) == 64);

enum class NodeKind : u32 { Assembly = 0, Part = 1, Body = 2 };

/// One row of the assembly tree, in depth-first order, so a subtree is a range of rows.
struct NodeRecord {
    u32 parent; // kNoParent for a root
    u32 subtreeEnd;
    u32 firstOccurrence;
    u32 occurrenceEnd;
    u32 nameOffset; // into the strings
    u32 nameLength;
    u32 kind;
    u32 reserved;
};
static_assert(sizeof(NodeRecord) == 32);

/// Geometry for one definition, at one tessellation level. Meshlets address vertices and triangles
/// relative to `firstVertex` and `firstTriangle`. Edge segments address the same vertices, and edge
/// chunks address segments relative to `firstEdgeSegment`.
struct DefinitionRecord {
    std::array<float, 3> boundsMin;
    std::array<float, 3> boundsMax;
    std::array<float, 3> decodeScale;
    std::array<float, 3> boundsScale;
    u32 definition; // index into the model's definitions
    u32 level;      // tessellation level, higher is finer
    u32 flags;
    u32 faceCount;
    u32 firstMeshlet;
    u32 meshletCount;
    u32 firstVertex;
    u32 vertexCount;
    u32 firstHighBits; // parallel to the definition's vertices, if it is wide
    u32 firstTriangle;
    u32 triangleCount;
    u32 firstFace;
    u32 firstEdgeChunk;
    u32 edgeChunkCount;
    u32 firstEdgeSegment;
    u32 edgeSegmentCount;
};
static_assert(sizeof(DefinitionRecord) == 112);

/// Per face: where it came from and its own colour.
struct FaceRecord {
    u32 sourceId; // the STEP entity id, or the importer's region number
    u32 color;    // RGBA8, or 0 for none
};
static_assert(sizeof(FaceRecord) == 8);

/// A validated view of a segment. Every span points into it.
struct SegmentView {
    std::span<const ModelInfo> modelInfo; // one record, in the model's segment
    std::span<const DefinitionInfo> definitionInfos;
    std::span<const OccurrenceRecord> occurrences;
    std::span<const NodeRecord> nodes;
    std::span<const char> strings;

    std::span<const DefinitionRecord> definitions;
    std::span<const Meshlet> meshlets;
    std::span<const PackedVertex> vertices;
    std::span<const u32> highBits;
    std::span<const u32> triangles;
    std::span<const FaceRecord> faces;
    std::span<const EdgeSegment> edgeSegments;
    std::span<const EdgeChunk> edgeChunks;

    bool hasModel() const { return !modelInfo.empty(); }
};

/// Validates a finished file's header and returns its segment table.
bool parseHeader(std::span<const u8> file, std::span<const SegmentEntry>& segments,
                 std::string& error);

/// Validates `bytes` as a segment and fills `view`. A cache file is untrusted input, because the
/// sandboxed helper that writes it parses untrusted files and could have been compromised. So every
/// size and offset the viewer relies on is checked here, and shaders clamp what is too costly to
/// check per element.
bool parseSegment(std::span<const u8> bytes, SegmentView& view, std::string& error);

} // namespace geo::format
