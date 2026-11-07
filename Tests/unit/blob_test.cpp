#include "cache/blob_writer.h"
#include "format/blob.h"
#include "import.h"
#include "mesh/indexed_mesh.h"

#include <cstdio>
#include <cstring>
#include <doctest/doctest.h>
#include <unistd.h>
#include <vector>

using namespace geo;
using namespace geo::format;

namespace {

constexpr std::string_view kTriangleStl =
    "solid s\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendsolid s\n";

struct Sample {
    std::vector<u8> file;

    SegmentEntry entry(usize index) const {
        return reinterpret_cast<const Header*>(file.data())->segments[index];
    }
    std::span<const u8> segment(usize index) const {
        const SegmentEntry e = entry(index);
        return std::span<const u8>(file).subspan(e.offset, e.size);
    }
    u8* mutableSegment(usize index) { return file.data() + entry(index).offset; }

    // The model is written first and geometry after it.
    usize modelIndex() const { return 0; }
    usize geometryIndex() const { return 1; }

    SectionEntry& section(usize segmentIndex, SectionKind kind) {
        auto* header = reinterpret_cast<SegmentHeader*>(mutableSegment(segmentIndex));
        for (u32 i = 0; i < header->sectionCount; ++i) {
            if (header->sections[i].kind == u32(kind)) {
                return header->sections[i];
            }
        }
        FAIL("missing section");
        return header->sections[0];
    }

    template <typename T>
    T& first(usize segmentIndex, SectionKind kind) {
        return *reinterpret_cast<T*>(mutableSegment(segmentIndex) +
                                     section(segmentIndex, kind).offset);
    }

    bool parses(usize index) const {
        SegmentView view;
        std::string error;
        return parseSegment(segment(index), view, error);
    }
};

Sample writeSample() {
    char path[] = "/tmp/geoblobXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::ImportResult result;
    std::string error;
    REQUIRE(
        import::importMesh({reinterpret_cast<const u8*>(kTriangleStl.data()), kTriangleStl.size()},
                           fd, nullptr, result, error));
    const off_t size = lseek(fd, 0, SEEK_END);
    Sample sample;
    sample.file.resize(usize(size));
    REQUIRE(pread(fd, sample.file.data(), sample.file.size(), 0) == size);
    close(fd);
    unlink(path);
    return sample;
}

} // namespace

TEST_CASE("a written file reads back as a model and geometry") {
    const Sample sample = writeSample();
    std::span<const SegmentEntry> segments;
    std::string error;
    REQUIRE(parseHeader(sample.file, segments, error));
    REQUIRE(segments.size() == 2);

    SegmentView model, geometry;
    REQUIRE(parseSegment(sample.segment(0), model, error));
    REQUIRE(parseSegment(sample.segment(1), geometry, error));
    CHECK(model.hasModel());
    CHECK(model.definitionInfos.size() == 1);
    CHECK(model.occurrences.size() == 1);
    CHECK(model.nodes.size() == 1);
    CHECK_FALSE(geometry.hasModel());
    CHECK(geometry.definitions.size() == 1);
    CHECK(geometry.triangles.size() == 1);
    CHECK(geometry.definitions[0].meshletCount == geometry.meshlets.size());
    CHECK(geometry.faces.size() == geometry.definitions[0].faceCount);
    CHECK(sample.file.size() % kAlignment == 0);
}

TEST_CASE("segments start and end on page boundaries") {
    const Sample sample = writeSample();
    std::span<const SegmentEntry> segments;
    std::string error;
    REQUIRE(parseHeader(sample.file, segments, error));
    for (const SegmentEntry& entry : segments) {
        CHECK(entry.offset % kAlignment == 0);
        CHECK(entry.size % kAlignment == 0);
    }
}

TEST_CASE("a damaged header is rejected") {
    const auto parses = [](const std::vector<u8>& file) {
        std::span<const SegmentEntry> segments;
        std::string error;
        return parseHeader(file, segments, error);
    };
    Sample sample = writeSample();
    sample.file[0] = 'X';
    CHECK_FALSE(parses(sample.file));

    sample = writeSample();
    reinterpret_cast<Header*>(sample.file.data())->version = kVersion + 1;
    CHECK_FALSE(parses(sample.file));

    sample = writeSample();
    sample.file.resize(sample.file.size() - 1);
    CHECK_FALSE(parses(sample.file));

    sample = writeSample();
    reinterpret_cast<Header*>(sample.file.data())->segments[0].size = u64(1) << 62;
    CHECK_FALSE(parses(sample.file));

    CHECK_FALSE(parses({}));
}

TEST_CASE("a damaged segment header is rejected") {
    Sample sample = writeSample();
    sample.mutableSegment(0)[0] = 'X';
    CHECK_FALSE(sample.parses(0));

    sample = writeSample();
    reinterpret_cast<SegmentHeader*>(sample.mutableSegment(0))->size += 1;
    CHECK_FALSE(sample.parses(0));
}

TEST_CASE("a section pointing outside its segment is rejected") {
    Sample sample = writeSample();
    sample.section(1, SectionKind::Vertices).size = u64(1) << 62;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.section(1, SectionKind::Vertices).offset = ~u64(0) - 15;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.section(1, SectionKind::Vertices).offset += 1;
    CHECK_FALSE(sample.parses(1));
}

TEST_CASE("a section with the wrong record size is rejected") {
    Sample sample = writeSample();
    sample.section(1, SectionKind::Meshlets).stride = 16;
    CHECK_FALSE(sample.parses(1));
}

TEST_CASE("a meshlet pointing outside its geometry is rejected") {
    Sample sample = writeSample();
    sample.first<Meshlet>(1, SectionKind::Meshlets).vertexOffset = 0xFFFFFFFF;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.first<Meshlet>(1, SectionKind::Meshlets).triangleOffset = 1000;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.first<Meshlet>(1, SectionKind::Meshlets).vertexCount = 200;
    CHECK_FALSE(sample.parses(1));
}

TEST_CASE("a definition pointing outside its geometry is rejected") {
    Sample sample = writeSample();
    sample.first<DefinitionRecord>(1, SectionKind::Definitions).meshletCount = 0xFFFFFFFF;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.first<DefinitionRecord>(1, SectionKind::Definitions).firstVertex = 0xFFFFFFF0;
    CHECK_FALSE(sample.parses(1));

    sample = writeSample();
    sample.first<DefinitionRecord>(1, SectionKind::Definitions).faceCount = 0xFFFFFFFF;
    CHECK_FALSE(sample.parses(1));
}

TEST_CASE("an occurrence of a missing definition is rejected") {
    Sample sample = writeSample();
    sample.first<OccurrenceRecord>(0, SectionKind::Occurrences).definition = 7;
    CHECK_FALSE(sample.parses(0));
}

TEST_CASE("a tree node that leaves the tree is rejected") {
    Sample sample = writeSample();
    sample.first<NodeRecord>(0, SectionKind::Nodes).subtreeEnd = 99;
    CHECK_FALSE(sample.parses(0));

    sample = writeSample();
    sample.first<NodeRecord>(0, SectionKind::Nodes).nameLength = 99;
    CHECK_FALSE(sample.parses(0));
}
