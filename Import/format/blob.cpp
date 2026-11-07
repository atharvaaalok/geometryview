#include "format/blob.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace geo::format {
namespace {

template <typename T>
bool readSection(std::span<const u8> file, const Header& header, SectionKind kind,
                 std::span<const T>& out, std::string& error) {
    out = {};
    for (u32 i = 0; i < header.sectionCount; ++i) {
        const SectionEntry& section = header.sections[i];
        if (section.kind != u32(kind)) {
            continue;
        }
        // Written so that no sum can overflow, because these values are untrusted.
        if (section.stride != sizeof(T) || section.offset % kAlignment != 0 ||
            section.offset > file.size() || section.size > file.size() - section.offset ||
            section.size % sizeof(T) != 0) {
            error = std::format("Section {} has an invalid layout.", u32(kind));
            return false;
        }
        out = {reinterpret_cast<const T*>(file.data() + section.offset), section.size / sizeof(T)};
        return true;
    }
    return true; // a missing section is simply empty
}

bool validateMeshlets(const BlobView& view, std::string& error) {
    const usize vertexCount = view.vertices.size();
    const usize triangleCount = view.triangles.size();
    for (const Meshlet& m : view.meshlets) {
        if (m.vertexCount > kMeshletMaxVertices || m.triangleCount > kMeshletMaxTriangles ||
            m.vertexOffset > vertexCount || m.vertexCount > vertexCount - m.vertexOffset ||
            m.triangleOffset > triangleCount ||
            m.triangleCount > triangleCount - m.triangleOffset ||
            (m.vertexCount == 0 && m.triangleCount != 0)) {
            error = "A meshlet points outside its geometry.";
            return false;
        }
    }
    return true;
}

bool validateDefinitions(const BlobView& view, std::string& error) {
    const usize meshletCount = view.meshlets.size();
    for (const DefinitionRecord& d : view.definitions) {
        if (d.firstMeshlet > meshletCount || d.meshletCount > meshletCount - d.firstMeshlet) {
            error = "A definition points outside its meshlets.";
            return false;
        }
        if (d.firstOriginalFace > view.originalFaces.size() ||
            d.faceCount > view.originalFaces.size() - d.firstOriginalFace) {
            error = "A definition points outside its face table.";
            return false;
        }
        if ((d.flags & kDefinitionWide) != 0 && view.highBits.size() != view.vertices.size()) {
            error = "A wide definition has no high bits.";
            return false;
        }
    }
    for (const OccurrenceRecord& o : view.occurrences) {
        if (o.definition >= view.definitions.size()) {
            error = "An occurrence points to a missing definition.";
            return false;
        }
    }
    return true;
}

} // namespace

bool parseBlob(std::span<const u8> file, BlobView& view, std::string& error) {
    if (file.size() < sizeof(Header)) {
        error = "The cache file is too short.";
        return false;
    }
    const auto* header = reinterpret_cast<const Header*>(file.data());
    if (header->magic != kMagic) {
        error = "This is not a Geometry cache file.";
        return false;
    }
    if (header->version != kVersion) {
        error = std::format("Unsupported cache version {}.", header->version);
        return false;
    }
    if (header->fileSize != file.size() || header->sectionCount > kMaxSections) {
        error = "The cache file is incomplete.";
        return false;
    }

    view = {};
    view.header = header;
    const bool sections =
        readSection(file, *header, SectionKind::Definitions, view.definitions, error) &&
        readSection(file, *header, SectionKind::Meshlets, view.meshlets, error) &&
        readSection(file, *header, SectionKind::Vertices, view.vertices, error) &&
        readSection(file, *header, SectionKind::HighBits, view.highBits, error) &&
        readSection(file, *header, SectionKind::Triangles, view.triangles, error) &&
        readSection(file, *header, SectionKind::Occurrences, view.occurrences, error) &&
        readSection(file, *header, SectionKind::OriginalFaces, view.originalFaces, error);
    return sections && validateMeshlets(view, error) && validateDefinitions(view, error);
}

} // namespace geo::format
