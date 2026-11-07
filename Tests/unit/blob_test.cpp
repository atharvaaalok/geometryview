#include "cache/blob_writer.h"
#include "format/blob.h"

#include <cstdio>
#include <cstring>
#include <doctest/doctest.h>
#include <unistd.h>
#include <vector>

using namespace geo;
using namespace geo::format;

namespace {

std::vector<u8> writeSample() {
    import::TriangleSoup soup;
    soup.corners = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const import::PackedDefinition packed = import::packMesh(import::buildIndexedMesh(soup));

    char path[] = "/tmp/geoblobXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    std::string error;
    REQUIRE(import::writeBlob(fd, packed, {}, error));
    const off_t size = lseek(fd, 0, SEEK_END);
    std::vector<u8> bytes(static_cast<usize>(size));
    REQUIRE(pread(fd, bytes.data(), bytes.size(), 0) == size);
    close(fd);
    unlink(path);
    return bytes;
}

SectionEntry& section(std::vector<u8>& file, SectionKind kind) {
    auto* header = reinterpret_cast<Header*>(file.data());
    for (u32 i = 0; i < header->sectionCount; ++i) {
        if (header->sections[i].kind == u32(kind)) {
            return header->sections[i];
        }
    }
    FAIL("missing section");
    return header->sections[0];
}

Meshlet& firstMeshlet(std::vector<u8>& file) {
    return *reinterpret_cast<Meshlet*>(file.data() + section(file, SectionKind::Meshlets).offset);
}

bool parses(const std::vector<u8>& file) {
    BlobView view;
    std::string error;
    return parseBlob(file, view, error);
}

} // namespace

TEST_CASE("a written blob reads back with every section") {
    const std::vector<u8> file = writeSample();
    BlobView view;
    std::string error;
    REQUIRE(parseBlob(file, view, error));
    CHECK(view.definitions.size() == 1);
    CHECK(view.occurrences.size() == 1);
    CHECK(view.triangles.size() == 2);
    CHECK(view.definitions[0].meshletCount == view.meshlets.size());
    CHECK(view.originalFaces.size() == view.definitions[0].faceCount);
    CHECK(file.size() % kAlignment == 0);
}

TEST_CASE("sections start on page boundaries") {
    std::vector<u8> file = writeSample();
    const auto* header = reinterpret_cast<const Header*>(file.data());
    for (u32 i = 0; i < header->sectionCount; ++i) {
        CHECK(header->sections[i].offset % kAlignment == 0);
    }
}

TEST_CASE("a damaged header is rejected") {
    std::vector<u8> file = writeSample();
    file[0] = 'X';
    CHECK_FALSE(parses(file));

    file = writeSample();
    reinterpret_cast<Header*>(file.data())->version = kVersion + 1;
    CHECK_FALSE(parses(file));

    file = writeSample();
    file.resize(file.size() - 1);
    CHECK_FALSE(parses(file));

    CHECK_FALSE(parses({}));
}

TEST_CASE("a section pointing outside the file is rejected") {
    std::vector<u8> file = writeSample();
    section(file, SectionKind::Vertices).size = u64(1) << 62;
    CHECK_FALSE(parses(file));

    file = writeSample();
    section(file, SectionKind::Vertices).offset = ~u64(0) - (kAlignment - 1);
    CHECK_FALSE(parses(file));

    file = writeSample();
    section(file, SectionKind::Vertices).offset += 1;
    CHECK_FALSE(parses(file));
}

TEST_CASE("a section with the wrong record size is rejected") {
    std::vector<u8> file = writeSample();
    section(file, SectionKind::Meshlets).stride = 16;
    CHECK_FALSE(parses(file));
}

TEST_CASE("a meshlet pointing outside its geometry is rejected") {
    std::vector<u8> file = writeSample();
    firstMeshlet(file).vertexOffset = 0xFFFFFFFF;
    CHECK_FALSE(parses(file));

    file = writeSample();
    firstMeshlet(file).triangleOffset = 1000;
    CHECK_FALSE(parses(file));

    file = writeSample();
    firstMeshlet(file).vertexCount = 200;
    CHECK_FALSE(parses(file));
}

TEST_CASE("a definition pointing outside its meshlets is rejected") {
    std::vector<u8> file = writeSample();
    auto* definition = reinterpret_cast<DefinitionRecord*>(
        file.data() + section(file, SectionKind::Definitions).offset);
    definition->meshletCount = 0xFFFFFFFF;
    CHECK_FALSE(parses(file));
}

TEST_CASE("an occurrence of a missing definition is rejected") {
    std::vector<u8> file = writeSample();
    auto* occurrence = reinterpret_cast<OccurrenceRecord*>(
        file.data() + section(file, SectionKind::Occurrences).offset);
    occurrence->definition = 7;
    CHECK_FALSE(parses(file));
}
