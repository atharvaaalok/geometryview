#include "import.h"

#include "base/time.h"
#include "cache/blob_writer.h"
#include "mesh/indexed_mesh.h"
#include "mesh/packer.h"
#include "stl/stl_reader.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstring>

namespace geo::import {

std::array<u8, 16> hashContents(std::span<const u8> file) {
    const XXH128_hash_t hash = XXH3_128bits(file.data(), file.size());
    std::array<u8, 16> bytes{};
    std::memcpy(bytes.data(), &hash, sizeof(hash));
    return bytes;
}

namespace {

// A mesh file holds one nameless part, with no structure.
ModelData singlePartModel(const PackedDefinition& packed) {
    ModelData model; // a mesh has no tolerances, so its info stays zero and it is never refined
    format::DefinitionInfo info{};
    info.boundsMin = packed.boundsMin;
    info.boundsMax = packed.boundsMax;
    info.faceCount = packed.faceCount;
    info.flags = packed.flags;
    model.definitions.push_back(info);

    format::OccurrenceRecord occurrence{};
    occurrence.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    model.occurrences.push_back(occurrence);

    format::NodeRecord node{};
    node.parent = format::kNoParent;
    node.subtreeEnd = 1;
    node.occurrenceEnd = 1;
    node.kind = u32(format::NodeKind::Part);
    model.nodes.push_back(node);
    return model;
}

} // namespace

bool importMesh(std::span<const u8> file, int outputFd, SegmentListener* listener,
                ImportResult& result, std::string& error) {
    double start = nowSeconds();
    const auto lap = [&](double& stage) {
        const double now = nowSeconds();
        stage = now - start;
        start = now;
    };

    TriangleSoup soup;
    if (!readStl(file, soup, error)) {
        return false;
    }
    result.warnings = std::move(soup.warnings);
    lap(result.times.parse);

    const IndexedMesh mesh = buildIndexedMesh(soup);
    lap(result.times.regions);

    PackedDefinition packed = packMesh(mesh);
    lap(result.times.pack);

    result.triangles = packed.triangles.size();
    result.faces = packed.faceCount;
    result.meshlets = packed.meshlets.size();
    result.wide = (packed.flags & kDefinitionWide) != 0;

    ModelData model = singlePartModel(packed);
    std::vector<format::FaceRecord> faces(packed.originalFace.size());
    std::transform(packed.originalFace.begin(), packed.originalFace.end(), faces.begin(),
                   [](u32 original) { return format::FaceRecord{original, 0}; });

    BlobStream stream(outputFd, listener);
    if (!stream.addDefinition(0, 0, std::move(packed), std::move(faces)) ||
        !stream.writeModel(model) || !stream.finish(hashContents(file))) {
        error = stream.error();
        return false;
    }
    lap(result.times.write);
    return true;
}

} // namespace geo::import
