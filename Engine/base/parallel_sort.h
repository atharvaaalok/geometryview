// A parallel sample sort for large arrays.
#pragma once

#include "base/parallel.h"
#include "base/types.h"

#include <algorithm>
#include <array>
#include <vector>

namespace geo {

/// Leaves items with equal keys in whatever order the standard library's sort gives them.
struct NoTieBreak {
    template <typename T>
    u32 operator()(const T&) const {
        return 0;
    }
};

/// Sorts `items` by the u64 returned by `keyOf`, and items with equal keys by the u32 returned by
/// `tieOf`. The result does not depend on the thread count, and, when `tieOf` tells every item
/// apart, not on the standard library either, so every platform builds identical cache files.
///
/// Splitters drawn from a sample divide items into buckets that can be sorted independently, which
/// stays balanced however keys are distributed. Splitting on top bits would not, because a mesh
/// with one face has identical high key bits.
template <typename T, typename KeyOf, typename TieOf = NoTieBreak>
void parallelSort(std::vector<T>& items, KeyOf&& keyOf, TieOf&& tieOf = {}) {
    constexpr usize kSerialLimit = 1 << 15;
    constexpr usize kBuckets = 256;
    constexpr usize kOversample = 32;
    constexpr usize kGrain = 1 << 15;

    const auto less = [&](const T& a, const T& b) {
        const u64 keyA = keyOf(a), keyB = keyOf(b);
        return keyA < keyB || (keyA == keyB && tieOf(a) < tieOf(b));
    };
    const usize count = items.size();
    if (count < kSerialLimit) {
        std::sort(items.begin(), items.end(), less);
        return;
    }

    std::vector<u64> sample(kBuckets * kOversample);
    for (usize i = 0; i < sample.size(); ++i) {
        sample[i] = keyOf(items[i * count / sample.size()]);
    }
    std::sort(sample.begin(), sample.end());
    std::array<u64, kBuckets - 1> splitters{};
    for (usize i = 0; i + 1 < kBuckets; ++i) {
        splitters[i] = sample[(i + 1) * kOversample];
    }
    const auto bucketOf = [&](const T& item) {
        return usize(std::upper_bound(splitters.begin(), splitters.end(), keyOf(item)) -
                     splitters.begin());
    };

    const usize ranges = parallelRangeCount(count, kGrain);
    std::vector<std::array<usize, kBuckets>> counts(ranges);
    parallelRanges(count, kGrain, [&](usize first, usize last, usize range) {
        std::array<usize, kBuckets>& local = counts[range];
        local.fill(0);
        for (usize i = first; i < last; ++i) {
            ++local[bucketOf(items[i])];
        }
    });

    // Each range writes from its own cursor per bucket, so scattering needs no synchronisation, and
    // items keep their relative order.
    std::array<usize, kBuckets + 1> bucketStarts{};
    std::vector<std::array<usize, kBuckets>> cursors(ranges);
    usize total = 0;
    for (usize b = 0; b < kBuckets; ++b) {
        bucketStarts[b] = total;
        for (usize range = 0; range < ranges; ++range) {
            cursors[range][b] = total;
            total += counts[range][b];
        }
    }
    bucketStarts[kBuckets] = total;

    std::vector<T> scattered(count);
    parallelRanges(count, kGrain, [&](usize first, usize last, usize range) {
        std::array<usize, kBuckets>& cursor = cursors[range];
        for (usize i = first; i < last; ++i) {
            scattered[cursor[bucketOf(items[i])]++] = items[i];
        }
    });

    parallelFor(kBuckets, [&](usize b) {
        std::sort(scattered.begin() + std::ptrdiff_t(bucketStarts[b]),
                  scattered.begin() + std::ptrdiff_t(bucketStarts[b + 1]), less);
    });
    items.swap(scattered);
}

} // namespace geo
