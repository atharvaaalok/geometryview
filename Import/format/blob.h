// The on-disk cache format. A blob is mapped read-only and its GPU sections are wrapped directly in
// Metal buffers, so geometry exists once in memory and is backed by a clean file.
//
// Layout: a 16 KB header, then sections aligned to 16 KB, the page size on Apple silicon. Everything
// is little-endian.
#pragma once

#include "base/types.h"
#include "format/gpu_records.h"

#include <array>
#include <span>
#include <string>

namespace geo::format {

inline constexpr std::array<char, 8> kMagic = {'G', 'E', 'O', 'B', 'L', 'O', 'B', '\0'};
inline constexpr u32 kVersion = 1;
inline constexpr usize kAlignment = 16384;
inline constexpr usize kMaxSections = 16;

enum class SectionKind : u32 {
    Definitions = 1,
    Meshlets = 2,
    Vertices = 3,
    HighBits = 4,
    Triangles = 5,
    Occurrences = 6,
    OriginalFaces = 7,
};

struct SectionEntry {
    u32 kind;
    u32 stride;
    u64 offset;
    u64 size;
};
static_assert(sizeof(SectionEntry) == 24);

struct Header {
    std::array<char, 8> magic;
    u32 version;
    u32 sectionCount;
    u64 fileSize;
    std::array<u8, 16> sourceHash;
    std::array<SectionEntry, kMaxSections> sections;
};

struct DefinitionRecord {
    std::array<float, 3> boundsMin;
    std::array<float, 3> boundsMax;
    std::array<float, 3> decodeScale;
    std::array<float, 3> boundsScale;
    u32 flags;
    u32 faceCount;
    u32 firstMeshlet;
    u32 meshletCount;
    u32 firstOriginalFace;
    u32 reserved[3];
};
static_assert(sizeof(DefinitionRecord) == 80);

struct OccurrenceRecord {
    std::array<float, 12> transform; // three columns of a 3x4 matrix, then translation
    u32 definition;
    u32 reserved[3];
};
static_assert(sizeof(OccurrenceRecord) == 64);

/// A validated view of a blob. Every span points into the mapping.
struct BlobView {
    const Header* header = nullptr;
    std::span<const DefinitionRecord> definitions;
    std::span<const Meshlet> meshlets;
    std::span<const PackedVertex> vertices;
    std::span<const u32> highBits;
    std::span<const u32> triangles;
    std::span<const OccurrenceRecord> occurrences;
    std::span<const u32> originalFaces;
};

/// Validates `file` and fills `view`. A blob is untrusted input, because the sandboxed helper that
/// writes it parses untrusted files and could have been compromised. So every size and offset the
/// viewer relies on is checked here, and shaders clamp what is too costly to check per element.
bool parseBlob(std::span<const u8> file, BlobView& view, std::string& error);

} // namespace geo::format
