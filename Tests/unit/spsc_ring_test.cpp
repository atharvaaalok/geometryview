#include "base/parallel.h"
#include "base/spsc_ring.h"

#include <doctest/doctest.h>
#include <thread>

using geo::SpscRing;
using geo::u64;

TEST_CASE("the ring returns elements in order and reports empty") {
    SpscRing<int, 4> ring;
    CHECK_FALSE(ring.tryPop().has_value());
    CHECK(ring.tryPush(1));
    CHECK(ring.tryPush(2));
    CHECK(ring.tryPop() == 1);
    CHECK(ring.tryPop() == 2);
    CHECK_FALSE(ring.tryPop().has_value());
}

TEST_CASE("a full ring rejects new elements until one is consumed") {
    SpscRing<int, 2> ring;
    CHECK(ring.tryPush(1));
    CHECK(ring.tryPush(2));
    CHECK_FALSE(ring.tryPush(3));
    CHECK(ring.tryPop() == 1);
    CHECK(ring.tryPush(3));
    CHECK(ring.tryPop() == 2);
    CHECK(ring.tryPop() == 3);
}

TEST_CASE("the ring keeps order across threads and wrap-around" *
          doctest::skip(!GEO_HAS_THREADS)) {
    SpscRing<u64, 64> ring;
    constexpr u64 count = 2'000'000;

    std::thread producer([&] {
        for (u64 i = 0; i < count;) {
            if (ring.tryPush(i)) {
                ++i;
            }
        }
    });

    u64 expected = 0;
    bool ordered = true;
    while (expected < count) {
        if (auto value = ring.tryPop()) {
            ordered = ordered && *value == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(ordered);
}
