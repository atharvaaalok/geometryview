// The parallel helpers: every index runs exactly once, also when two threads post work at the same
// time, as the packer does while it finds edges alongside its meshlets.
#include "base/parallel.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <vector>

using namespace geo;

namespace {

// Runs parallelFor over `count` indices, counting each visit.
std::vector<int> visits(usize count) {
    std::vector<std::atomic<int>> seen(count);
    parallelFor(count, [&](usize i) { seen[i].fetch_add(1, std::memory_order_relaxed); });
    std::vector<int> result(count);
    for (usize i = 0; i < count; ++i) {
        result[i] = seen[i].load();
    }
    return result;
}

} // namespace

TEST_CASE("parallelFor runs every index once") {
    for (const usize count : {usize(0), usize(1), usize(7), usize(10000)}) {
        const std::vector<int> seen = visits(count);
        CHECK(std::count(seen.begin(), seen.end(), 1) == std::ptrdiff_t(count));
    }
}

TEST_CASE("work posted from two threads at once all runs") {
    struct Side {
        std::vector<int> seen;
        int rounds = 0;
    };
    // Many rounds, so the two posts overlap in every way the pool can interleave them.
    Side other;
    const auto postMany = [](void* context) {
        auto* side = static_cast<Side*>(context);
        for (; side->rounds < 200; ++side->rounds) {
            side->seen = visits(1000 + usize(side->rounds));
        }
    };
    Concurrent task;
    task.start(&other, postMany);
    Side mine;
    postMany(&mine);
    task.wait();
    CHECK(mine.rounds == 200);
    CHECK(other.rounds == 200);
    CHECK(std::count(mine.seen.begin(), mine.seen.end(), 1) == 1199);
    CHECK(std::count(other.seen.begin(), other.seen.end(), 1) == 1199);
}

TEST_CASE("parallelRanges covers its range in order-independent pieces") {
    std::vector<std::atomic<int>> seen(100000);
    std::atomic<usize> ranges{0};
    parallelRanges(seen.size(), 1000, [&](usize first, usize last, usize) {
        ranges.fetch_add(1);
        for (usize i = first; i < last; ++i) {
            seen[i].fetch_add(1, std::memory_order_relaxed);
        }
    });
    CHECK(ranges.load() == parallelRangeCount(seen.size(), 1000));
    for (const std::atomic<int>& s : seen) {
        REQUIRE(s.load() == 1);
    }
}
