#include "import.h"

#include "base/time.h"
#include "cache/blob_writer.h"
#include "mesh/indexed_mesh.h"
#include "mesh/packer.h"
#include "stl/stl_reader.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstring>

namespace geo::import {

std::array<u8, 16> hashContents(std::span<const u8> file) {
    const XXH128_hash_t hash = XXH3_128bits(file.data(), file.size());
    std::array<u8, 16> bytes{};
    std::memcpy(bytes.data(), &hash, sizeof(hash));
    return bytes;
}

bool importMesh(std::span<const u8> file, int outputFd, ImportResult& result, std::string& error) {
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

    const PackedDefinition packed = packMesh(mesh);
    lap(result.times.pack);

    if (!writeBlob(outputFd, packed, hashContents(file), error)) {
        return false;
    }
    lap(result.times.write);

    result.triangles = packed.triangles.size();
    result.faces = packed.faceCount;
    result.meshlets = packed.meshlets.size();
    result.wide = (packed.flags & kDefinitionWide) != 0;
    return true;
}

} // namespace geo::import
