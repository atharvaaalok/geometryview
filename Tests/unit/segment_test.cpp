// Cache segments held in memory rather than mapped, as in the browser, and a cache file's header
// read from its start before the rest has arrived.
#include "cache/blob_writer.h"
#include "scene/segment.h"

#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

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
