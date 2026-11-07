#include "base/parallel_sort.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <random>

using namespace geo;

namespace {

void checkSorts(std::vector<u64> items) {
    std::vector<u64> expected = items;
    std::sort(expected.begin(), expected.end());
    parallelSort(items, [](u64 value) { return value; });
    CHECK(items == expected);
}

} // namespace

TEST_CASE("parallel sort handles small and empty inputs") {
    checkSorts({});
    checkSorts({3, 1, 2});
}

TEST_CASE("parallel sort matches std::sort on random keys") {
    std::mt19937_64 random(1);
    std::vector<u64> items(1 << 20);
    std::generate(items.begin(), items.end(), [&] { return random(); });
    checkSorts(std::move(items));
}

TEST_CASE("parallel sort copes with keys that share their high bits") {
    std::mt19937_64 random(2);
    std::vector<u64> items(1 << 18);
    std::generate(items.begin(), items.end(), [&] { return random() & 0x3FFFFFFF; });
    checkSorts(std::move(items));
}

TEST_CASE("parallel sort copes with mostly duplicate keys") {
    std::vector<u64> items(1 << 18, 7);
    items[100] = 3;
    items[200] = 9;
    checkSorts(std::move(items));
}

TEST_CASE("parallel sort copes with already sorted and reversed input") {
    std::vector<u64> items(1 << 18);
    for (usize i = 0; i < items.size(); ++i) {
        items[i] = i;
    }
    checkSorts(items);
    std::reverse(items.begin(), items.end());
    checkSorts(items);
}
