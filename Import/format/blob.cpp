#include "format/blob.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

namespace geo::format {
namespace {

template <typename T>
bool readSection(const SegmentHeader& header, u64 segmentSize, const SectionBytes& bytes,
                 SectionKind kind, std::span<const T>& out, SegmentView& view, std::string& error) {
    out = {};
    for (u32 i = 0; i < header.sectionCount; ++i) {
        const SectionEntry& section = header.sections[i];
        if (section.kind != u32(kind)) {
            continue;
        }
        // Written so that no sum can overflow, because these values are untrusted.
        if (section.stride != sizeof(T) || section.offset % alignof(T) != 0 ||
            section.offset > segmentSize || section.size > segmentSize - section.offset ||
            section.size % sizeof(T) != 0) {
            error = std::format("Section {} has an invalid layout.", u32(kind));
            return false;
        }
        const u64 count = section.size / sizeof(T);
        view.places[usize(kind)] = {section.offset, count};
        const u8* memory = bytes[usize(kind)];
        if (memory == nullptr) {
            if (readOnCpu(kind) && count != 0) {
                error = std::format("Section {} is not in memory.", u32(kind));
                return false;
            }
            return true;
        }
        if (reinterpret_cast<uintptr_t>(memory) % alignof(T) != 0) {
            error = std::format("Section {} is misaligned in memory.", u32(kind));
            return false;
        }
        // The count fits in usize, the section having been checked against the segment's size.
        out = {reinterpret_cast<const T*>(memory), usize(count)};
        return true;
    }
    return true; // a missing section is simply empty
}

bool validateModel(const SegmentView& view, std::string& error) {
    if (view.modelInfo.size() > 1) {
        error = "A segment has two models.";
        return false;
    }
    for (const OccurrenceRecord& o : view.occurrences) {
        if (o.definition >= view.definitionInfos.size() || o.node >= view.nodes.size()) {
            error = "An occurrence points to something that does not exist.";
            return false;
        }
    }
    const usize nodeCount = view.nodes.size();
    const usize occurrenceCount = view.occurrences.size();
    for (usize n = 0; n < nodeCount; ++n) {
        const NodeRecord& node = view.nodes[n];
        if ((node.parent != kNoParent && node.parent >= n) || node.subtreeEnd <= n ||
            node.subtreeEnd > nodeCount || node.firstOccurrence > node.occurrenceEnd ||
            node.occurrenceEnd > occurrenceCount || node.nameOffset > view.strings.size() ||
            node.nameLength > view.strings.size() - node.nameOffset) {
            error = "A tree node is invalid.";
            return false;
        }
    }
    return true;
}

// Sections held only on the GPU are checked by their counts.
bool validateDefinition(const SegmentView& view, const DefinitionRecord& d, std::string& error) {
    const u64 vertices = view.count(SectionKind::Vertices);
    const u64 triangles = view.count(SectionKind::Triangles);
    const u64 faces = view.count(SectionKind::Faces);
    const u64 edgeSegments = view.count(SectionKind::EdgeSegments);
    const u64 highBits = view.count(SectionKind::HighBits);
    if (d.firstMeshlet > view.meshlets.size() ||
        d.meshletCount > view.meshlets.size() - d.firstMeshlet || d.firstVertex > vertices ||
        d.vertexCount > vertices - d.firstVertex || d.firstTriangle > triangles ||
        d.triangleCount > triangles - d.firstTriangle) {
        error = "A definition points outside its geometry.";
        return false;
    }
    if (d.firstFace > faces || d.faceCount > faces - d.firstFace) {
        error = "A definition points outside its face table.";
        return false;
    }
    if (d.firstEdgeChunk > view.edgeChunks.size() ||
        d.edgeChunkCount > view.edgeChunks.size() - d.firstEdgeChunk ||
        d.firstEdgeSegment > edgeSegments ||
        d.edgeSegmentCount > edgeSegments - d.firstEdgeSegment) {
        error = "A definition points outside its edges.";
        return false;
    }
    if ((d.flags & kDefinitionWide) != 0 &&
        (d.firstHighBits > highBits || d.vertexCount > highBits - d.firstHighBits)) {
        error = "A wide definition has no high bits.";
        return false;
    }
    for (u32 i = 0; i < d.meshletCount; ++i) {
        const Meshlet& m = view.meshlets[d.firstMeshlet + i];
        if (m.vertexCount > kMeshletMaxVertices || m.triangleCount > kMeshletMaxTriangles ||
            m.vertexOffset > d.vertexCount || m.vertexCount > d.vertexCount - m.vertexOffset ||
            m.triangleOffset > d.triangleCount ||
            m.triangleCount > d.triangleCount - m.triangleOffset ||
            (m.vertexCount == 0 && m.triangleCount != 0)) {
            error = "A meshlet points outside its geometry.";
            return false;
        }
    }
    // Segments' vertices and faces are clamped by the shaders instead, because checking them here
    // would read every page of edges.
    for (u32 i = 0; i < d.edgeChunkCount; ++i) {
        const EdgeChunk& c = view.edgeChunks[d.firstEdgeChunk + i];
        if (c.segmentCount > kEdgeChunkSegments || c.firstSegment > d.edgeSegmentCount ||
            c.segmentCount > d.edgeSegmentCount - c.firstSegment) {
            error = "An edge chunk points outside its edges.";
            return false;
        }
    }
    return true;
}

} // namespace

bool parseHeaderPrefix(std::span<const u8> prefix, std::span<const SegmentEntry>& segments,
                       u64& fileSize, std::string& error) {
    if (prefix.size() < kAlignment) {
        error = "The cache file is too short.";
        return false;
    }
    Header header;
    std::memcpy(&header, prefix.data(), sizeof(header));
    if (header.magic != kMagic) {
        error = "This is not a GeometryView cache file.";
        return false;
    }
    if (header.version != kVersion) {
        error = std::format("Unsupported cache version {}.", header.version);
        return false;
    }
    if (header.segmentCount > kMaxSegments || header.fileSize < kAlignment) {
        error = "The cache file is not valid.";
        return false;
    }
    const auto* base = reinterpret_cast<const Header*>(prefix.data());
    segments = {base->segments.data(), header.segmentCount};
    for (const SegmentEntry& entry : segments) {
        if (entry.offset % kAlignment != 0 || entry.size % kAlignment != 0 || entry.size == 0 ||
            entry.offset > header.fileSize || entry.size > header.fileSize - entry.offset) {
            error = "A segment lies outside the cache file.";
            return false;
        }
    }
    fileSize = header.fileSize;
    return true;
}

bool parseHeader(std::span<const u8> file, std::span<const SegmentEntry>& segments,
                 std::string& error) {
    u64 fileSize = 0;
    if (!parseHeaderPrefix(file, segments, fileSize, error)) {
        return false;
    }
    if (fileSize != file.size()) {
        error = "The cache file is incomplete.";
        return false;
    }
    return true;
}

bool readOnCpu(SectionKind kind) {
    switch (kind) {
    case SectionKind::Vertices:
    case SectionKind::HighBits:
    case SectionKind::Triangles:
    case SectionKind::Faces:
    case SectionKind::EdgeSegments:
        return false;
    default:
        return true;
    }
}

bool parseSegmentParts(const SegmentHeader& header, u64 size, const SectionBytes& bytes,
                       SegmentView& view, std::string& error) {
    if (header.magic != kSegmentMagic || header.version != kVersion || header.size != size ||
        header.sectionCount > kMaxSections) {
        error = "A cache segment is damaged.";
        return false;
    }
    view = {};
    const auto read = [&](SectionKind kind, auto& span) {
        return readSection(header, size, bytes, kind, span, view, error);
    };
    const bool sections =
        read(SectionKind::ModelInfo, view.modelInfo) &&
        read(SectionKind::DefinitionInfos, view.definitionInfos) &&
        read(SectionKind::Occurrences, view.occurrences) && read(SectionKind::Nodes, view.nodes) &&
        read(SectionKind::Strings, view.strings) &&
        read(SectionKind::Definitions, view.definitions) &&
        read(SectionKind::Meshlets, view.meshlets) && read(SectionKind::Vertices, view.vertices) &&
        read(SectionKind::HighBits, view.highBits) &&
        read(SectionKind::Triangles, view.triangles) && read(SectionKind::Faces, view.faces) &&
        read(SectionKind::EdgeSegments, view.edgeSegments) &&
        read(SectionKind::EdgeChunks, view.edgeChunks);
    if (!sections || !validateModel(view, error)) {
        return false;
    }
    return std::all_of(
        view.definitions.begin(), view.definitions.end(),
        [&](const DefinitionRecord& d) { return validateDefinition(view, d, error); });
}

bool parseSegment(std::span<const u8> bytes, SegmentView& view, std::string& error) {
    if (bytes.size() < sizeof(SegmentHeader)) {
        error = "A cache segment is too short.";
        return false;
    }
    SegmentHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.size != bytes.size() || header.sectionCount > kMaxSections) {
        error = "A cache segment is damaged.";
        return false;
    }
    // Every section is in the segment itself, at its own offset.
    SectionBytes memory{};
    for (u32 i = 0; i < header.sectionCount; ++i) {
        const SectionEntry& section = header.sections[i];
        // The first section of a kind is the one read, as readSection reads it.
        if (section.kind < kSectionKinds && section.offset <= bytes.size() &&
            memory[section.kind] == nullptr) {
            memory[section.kind] = bytes.data() + usize(section.offset);
        }
    }
    return parseSegmentParts(header, bytes.size(), memory, view, error);
}

} // namespace geo::format
