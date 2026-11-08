// Cache segments held in memory rather than mapped, as in the browser, and a cache file's header
// read from its start before the rest has arrived.
#include "cache/blob_writer.h"
#include "import.h"
#include "scene/segment.h"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace geo;
using format::NodeKind;

namespace {

// A small finished cache file, as bytes, written the way the importer writes one.
std::vector<u8> cacheFile() {
    import::ModelData model;
    model.definitions = {{{0, 0, 0}, {1, 1, 1}, 3, 0}};
    model.occurrences = {{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 0, 1, 0, 0}};
    model.nodes = {{format::kNoParent, 2, 0, 1, 0, 0, u32(NodeKind::Assembly), 0},
                   {0, 2, 0, 1, 0, 0, u32(NodeKind::Part), 0}};
    char path[] = "/tmp/geosegmentXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::BlobStream stream(fd, nullptr);
    REQUIRE(stream.writeModel(model));
    REQUIRE(stream.finish({}));
    std::vector<u8> bytes(usize(lseek(fd, 0, SEEK_END)));
    REQUIRE(pread(fd, bytes.data(), bytes.size(), 0) == ssize_t(bytes.size()));
    close(fd);
    unlink(path);
    return bytes;
}

} // namespace

TEST_CASE("a cache file in memory reads as its mapping does") {
    const std::vector<u8> file = cacheFile();
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    REQUIRE_MESSAGE(scene::readCacheFile(file, segments, error), error);
    REQUIRE(!segments.empty());
    const scene::Segment& model = *segments[0];
    CHECK(!model.mapped());
    CHECK(model.view().hasModel());
    CHECK(model.view().nodes.size() == 2);
    CHECK(model.view().occurrences.size() == 1);
    // Aligned as a mapping would be, so any renderer can wrap it without copying again.
    CHECK(reinterpret_cast<uintptr_t>(model.data()) % format::kAlignment == 0);
}

TEST_CASE("a header read from the start lets each segment be read as its bytes arrive") {
    const std::vector<u8> file = cacheFile();
    const std::span<const u8> prefix(file.data(), format::kAlignment);
    std::span<const format::SegmentEntry> entries;
    u64 fileSize = 0;
    std::string error;
    REQUIRE_MESSAGE(format::parseHeaderPrefix(prefix, entries, fileSize, error), error);
    CHECK(fileSize == file.size());
    REQUIRE(!entries.empty());
    for (const format::SegmentEntry& entry : entries) {
        const std::shared_ptr<scene::Segment> segment = scene::Segment::copy(
            std::span(file).subspan(usize(entry.offset), usize(entry.size)), error);
        CHECK_MESSAGE(segment != nullptr, error);
    }
}

TEST_CASE("a cache file cut short is refused whole, but its header still reads") {
    const std::vector<u8> file = cacheFile();
    const std::span<const u8> cut(file.data(), file.size() - format::kAlignment);
    std::vector<std::shared_ptr<scene::Segment>> segments;
    std::string error;
    CHECK(!scene::readCacheFile(cut, segments, error));
    CHECK(error == "The cache file is incomplete.");
    std::span<const format::SegmentEntry> entries;
    u64 fileSize = 0;
    CHECK(format::parseHeaderPrefix(cut, entries, fileSize, error));
    CHECK(fileSize == file.size());
}

TEST_CASE("bytes that are not a whole number of pages are not a segment") {
    const std::vector<u8> file = cacheFile();
    std::string error;
    CHECK(scene::Segment::copy(std::span(file).subspan(0, 100), error) == nullptr);
    CHECK(scene::Segment::copy({}, error) == nullptr);
}

namespace {

// A cache file with a model and geometry: two triangles, imported as the STL reader imports them.
std::vector<u8> meshCacheFile() {
    constexpr std::string_view stl = "solid s\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                                     "vertex 1 0 0\nvertex 1 1 0\nvertex 0 1 0\nendsolid s\n";
    char path[] = "/tmp/geostreamXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    import::ImportResult result;
    std::string error;
    REQUIRE(import::importMesh({reinterpret_cast<const u8*>(stl.data()), stl.size()}, fd, nullptr,
                               result, error));
    std::vector<u8> bytes(usize(lseek(fd, 0, SEEK_END)));
    REQUIRE(pread(fd, bytes.data(), bytes.size(), 0) == ssize_t(bytes.size()));
    close(fd);
    unlink(path);
    return bytes;
}

// Keeps placed segments' bytes, as a renderer would on the GPU.
struct Placed : scene::Placement {
    std::vector<u8> bytes;
};
struct TestSink : scene::SegmentSink {
    std::vector<std::weak_ptr<Placed>> placed;
    u64 expected = 0;
    bool expectedFirst = false;
    void expect(u64 size) override {
        expected = size;
        expectedFirst = placed.empty();
    }
    std::shared_ptr<scene::Placement> place(u64 size) override {
        auto placement = std::make_shared<Placed>();
        placement->bytes.assign(usize(size), 0xEE);
        placed.push_back(placement);
        return placement;
    }
    void write(scene::Placement& placement, u64 offset, std::span<const u8> bytes) override {
        std::vector<u8>& to = static_cast<Placed&>(placement).bytes;
        REQUIRE(offset + bytes.size() <= to.size());
        std::memcpy(to.data() + offset, bytes.data(), bytes.size());
    }
};

// Streams `file` in pieces of `piece` bytes.
std::vector<std::shared_ptr<scene::Segment>> stream(const std::vector<u8>& file, usize piece,
                                                    scene::SegmentSink* sink) {
    std::vector<std::shared_ptr<scene::Segment>> segments;
    scene::CacheStream reader(sink, [&](std::shared_ptr<scene::Segment> segment) {
        segments.push_back(std::move(segment));
    });
    std::string error;
    for (usize at = 0; at < file.size(); at += piece) {
        REQUIRE_MESSAGE(
            reader.append(std::span(file).subspan(at, std::min(piece, file.size() - at)), error),
            error);
    }
    REQUIRE_MESSAGE(reader.finish(error), error);
    CHECK(reader.size() == file.size());
    return segments;
}

} // namespace

TEST_CASE("a streamed cache file keeps only what the CPU reads, and places the rest") {
    const std::vector<u8> file = meshCacheFile();
    std::vector<std::shared_ptr<scene::Segment>> whole;
    std::string error;
    REQUIRE_MESSAGE(scene::readCacheFile(file, whole, error), error);
    REQUIRE(whole.size() == 2);

    for (const usize piece : {usize(3), usize(7), usize(4096), usize(16384), file.size()}) {
        CAPTURE(piece);
        TestSink sink;
        const std::vector<std::shared_ptr<scene::Segment>> segments = stream(file, piece, &sink);
        REQUIRE(segments.size() == whole.size());
        // The file's size is announced before anything is placed, so room is made once.
        CHECK(sink.expected == file.size());
        CHECK(sink.expectedFirst);
        for (usize i = 0; i < segments.size(); ++i) {
            const scene::Segment& got = *segments[i];
            const scene::Segment& want = *whole[i];
            CHECK(got.data() == nullptr);
            REQUIRE(got.placement() != nullptr);
            // The placement holds the segment's bytes exactly.
            const std::vector<u8>& placed = static_cast<const Placed*>(got.placement())->bytes;
            CHECK(placed.size() == want.size());
            CHECK(std::memcmp(placed.data(), want.data(), want.size()) == 0);
            // Memory holds what the CPU reads, and knows where everything else is.
            const format::SegmentView& a = got.view();
            const format::SegmentView& b = want.view();
            CHECK(a.places == b.places);
            CHECK(a.hasModel() == b.hasModel());
            CHECK(a.nodes.size() == b.nodes.size());
            CHECK(a.definitions.size() == b.definitions.size());
            CHECK(a.meshlets.size() == b.meshlets.size());
            CHECK(a.edgeChunks.size() == b.edgeChunks.size());
            CHECK(a.vertices.empty());
            CHECK(a.triangles.empty());
            if (!b.meshlets.empty()) {
                CHECK(std::memcmp(a.meshlets.data(), b.meshlets.data(), b.meshlets.size_bytes()) ==
                      0);
            }
        }
    }
}

TEST_CASE("without a sink a streamed cache file is copied whole") {
    const std::vector<u8> file = meshCacheFile();
    const std::vector<std::shared_ptr<scene::Segment>> segments = stream(file, 5000, nullptr);
    REQUIRE(segments.size() == 2);
    for (const std::shared_ptr<scene::Segment>& segment : segments) {
        CHECK(segment->data() != nullptr);
        CHECK(segment->placement() == nullptr);
    }
    CHECK(!segments[1]->view().triangles.empty());
}

TEST_CASE("a streamed cache file that is cut short or damaged is refused") {
    const std::vector<u8> file = meshCacheFile();
    std::string error;
    {
        TestSink sink;
        scene::CacheStream reader(&sink, [](std::shared_ptr<scene::Segment>) {});
        REQUIRE(reader.append(std::span(file).first(file.size() - 100), error));
        CHECK(!reader.finish(error));
        CHECK(error == "The cache file ended early.");
    }
    {
        // A segment's header damaged: refused as its header arrives, and nothing taken after.
        std::vector<u8> damaged = file;
        std::span<const format::SegmentEntry> entries;
        u64 fileSize = 0;
        REQUIRE(format::parseHeaderPrefix(damaged, entries, fileSize, error));
        damaged[usize(entries[1].offset)] ^= 0xFF;
        TestSink sink;
        usize ready = 0;
        scene::CacheStream reader(&sink, [&](std::shared_ptr<scene::Segment>) { ++ready; });
        CHECK(!reader.append(damaged, error));
        CHECK(!reader.append(std::span(damaged).first(1), error));
        CHECK(ready < 2);
    }
    {
        TestSink sink;
        scene::CacheStream reader(&sink, [](std::shared_ptr<scene::Segment>) {});
        std::vector<u8> longer = file;
        longer.resize(file.size() + 10);
        CHECK(!reader.append(longer, error));
    }
}

TEST_CASE("a streamed cache file must be as large as the host knows it to be") {
    const std::vector<u8> file = meshCacheFile();
    std::string error;
    TestSink sink;
    scene::CacheStream reader(&sink, [](std::shared_ptr<scene::Segment>) {});
    reader.expectSize(file.size() + format::kAlignment);
    CHECK(!reader.append(file, error));
    CHECK(error == "The cache file's size does not match its header.");
    CHECK(sink.placed.empty());
}
