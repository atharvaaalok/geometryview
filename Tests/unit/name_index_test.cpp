#include "base/time.h"
#include "cache/blob_writer.h"
#include "model/name_index.h"
#include "scene/mapped_segment.h"

#include <cstdio>
#include <doctest/doctest.h>
#include <format>
#include <unistd.h>
#include <vector>

using namespace geo;

namespace {

// A flat model with one root and the given names under it.
struct Names {
    std::shared_ptr<scene::MappedSegment> segment;

    explicit Names(const std::vector<std::string>& names) {
        import::ModelData model;
        for (usize i = 0; i < names.size(); ++i) {
            format::NodeRecord node{};
            node.parent = i == 0 ? format::kNoParent : 0;
            node.subtreeEnd = i == 0 ? u32(names.size()) : u32(i + 1);
            node.nameOffset = u32(model.strings.size());
            node.nameLength = u32(names[i].size());
            model.strings += names[i];
            model.nodes.push_back(node);
        }
        char path[] = "/tmp/geonamesXXXXXX";
        const int fd = mkstemp(path);
        REQUIRE(fd >= 0);
        import::BlobStream stream(fd, nullptr);
        REQUIRE(stream.writeModel(model));
        REQUIRE(stream.finish({}));
        std::vector<std::shared_ptr<scene::MappedSegment>> segments;
        std::string error;
        REQUIRE(scene::openCacheFile(fd, segments, error));
        close(fd);
        unlink(path);
        segment = segments.front();
    }
};

std::vector<u32> find(const model::NameIndex& index, std::string_view query, u32& total) {
    std::vector<u32> results(16);
    total = index.search(query, results);
    results.resize(std::min<usize>(total, results.size()));
    return results;
}

} // namespace

TEST_CASE("search ignores case and finds each node once") {
    Names names({"Rig", "Bearing", "bearing housing", "Shaft", "BEARINGBEARING", ""});
    const model::NameIndex index(names.segment->view());
    u32 total = 0;
    CHECK(find(index, "bearing", total) == std::vector<u32>{1, 2, 4});
    CHECK(total == 3);
    CHECK(find(index, "SHAFT", total) == std::vector<u32>{3});
    CHECK(find(index, "missing", total).empty());
    CHECK(find(index, "", total).empty());
}

TEST_CASE("a match never spans two names") {
    Names names({"ab", "cd"});
    const model::NameIndex index(names.segment->view());
    u32 total = 0;
    CHECK(find(index, "bc", total).empty());
}

TEST_CASE("results beyond the buffer are counted but not written") {
    std::vector<std::string> list = {"root"};
    for (int i = 0; i < 40; ++i) {
        list.push_back(std::format("bolt {}", i));
    }
    Names names(list);
    const model::NameIndex index(names.segment->view());
    u32 total = 0;
    CHECK(find(index, "bolt", total).size() == 16);
    CHECK(total == 40);
}

TEST_CASE("searching a million names fits in a frame") {
    std::vector<std::string> list;
    list.reserve(1000000);
    for (int i = 0; i < 1000000; ++i) {
        list.push_back(std::format("Fastener M{} DIN 912 {}", i % 24, i));
    }
    Names names(list);
    const model::NameIndex index(names.segment->view());
    std::vector<u32> results(64);
    const double start = nowSeconds();
    const u32 total = index.search("din 912 99999", results);
    const double elapsed = nowSeconds() - start;
    MESSAGE(std::format("searched a million names in {:.2f} ms", elapsed * 1e3));
    CHECK(total == 11); // 99999 and 999990 to 999999
#ifdef NDEBUG
    CHECK(elapsed < 0.008);
#endif
}
