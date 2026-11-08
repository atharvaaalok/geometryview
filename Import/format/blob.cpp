#include "format/blob.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace geo::format {
namespace {

template <typename T>
bool readSection(std::span<const u8> segment, const SegmentHeader& header, SectionKind kind,
                 std::span<const T>& out, std::string& error) {
    out = {};
    for (u32 i = 0; i < header.sectionCount; ++i) {
        const SectionEntry& section = header.sections[i];
        if (section.kind != u32(kind)) {
            continue;
        }
        // Written so that no sum can overflow, because these values are untrusted.
        if (section.stride != sizeof(T) || section.offset % alignof(T) != 0 ||
            section.offset > segment.size() || section.size > segment.size() - section.offset ||
            section.size % sizeof(T) != 0) {
            error = std::format("Section {} has an invalid layout.", u32(kind));
            return false;
        }
        // Both fit in usize, having been checked against the segment's size above.
        out = {reinterpret_cast<const T*>(segment.data() + usize(section.offset)),
               usize(section.size / sizeof(T))};
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

bool validateDefinition(const SegmentView& view, const DefinitionRecord& d, std::string& error) {
    if (d.firstMeshlet > view.meshlets.size() ||
        d.meshletCount > view.meshlets.size() - d.firstMeshlet ||
        d.firstVertex > view.vertices.size() ||
        d.vertexCount > view.vertices.size() - d.firstVertex ||
        d.firstTriangle > view.triangles.size() ||
        d.triangleCount > view.triangles.size() - d.firstTriangle) {
        error = "A definition points outside its geometry.";
        return false;
    }
    if (d.firstFace > view.faces.size() || d.faceCount > view.faces.size() - d.firstFace) {
        error = "A definition points outside its face table.";
        return false;
    }
    if (d.firstEdgeChunk > view.edgeChunks.size() ||
        d.edgeChunkCount > view.edgeChunks.size() - d.firstEdgeChunk ||
        d.firstEdgeSegment > view.edgeSegments.size() ||
        d.edgeSegmentCount > view.edgeSegments.size() - d.firstEdgeSegment) {
        error = "A definition points outside its edges.";
        return false;
    }
    if ((d.flags & kDefinitionWide) != 0 &&
        (d.firstHighBits > view.highBits.size() ||
         d.vertexCount > view.highBits.size() - d.firstHighBits)) {
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
        error = "This is not a Geometry cache file.";
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

bool parseSegment(std::span<const u8> bytes, SegmentView& view, std::string& error) {
    if (bytes.size() < sizeof(SegmentHeader)) {
        error = "A cache segment is too short.";
        return false;
    }
    const auto* header = reinterpret_cast<const SegmentHeader*>(bytes.data());
    if (header->magic != kSegmentMagic || header->version != kVersion ||
        header->size != bytes.size() || header->sectionCount > kMaxSections) {
        error = "A cache segment is damaged.";
        return false;
    }

    view = {};
    const bool sections =
        readSection(bytes, *header, SectionKind::ModelInfo, view.modelInfo, error) &&
        readSection(bytes, *header, SectionKind::DefinitionInfos, view.definitionInfos, error) &&
        readSection(bytes, *header, SectionKind::Occurrences, view.occurrences, error) &&
        readSection(bytes, *header, SectionKind::Nodes, view.nodes, error) &&
        readSection(bytes, *header, SectionKind::Strings, view.strings, error) &&
        readSection(bytes, *header, SectionKind::Definitions, view.definitions, error) &&
        readSection(bytes, *header, SectionKind::Meshlets, view.meshlets, error) &&
        readSection(bytes, *header, SectionKind::Vertices, view.vertices, error) &&
        readSection(bytes, *header, SectionKind::HighBits, view.highBits, error) &&
        readSection(bytes, *header, SectionKind::Triangles, view.triangles, error) &&
        readSection(bytes, *header, SectionKind::Faces, view.faces, error) &&
        readSection(bytes, *header, SectionKind::EdgeSegments, view.edgeSegments, error) &&
        readSection(bytes, *header, SectionKind::EdgeChunks, view.edgeChunks, error);
    if (!sections || !validateModel(view, error)) {
        return false;
    }
    return std::all_of(
        view.definitions.begin(), view.definitions.end(),
        [&](const DefinitionRecord& d) { return validateDefinition(view, d, error); });
}

} // namespace geo::format
