// Parallel de-duplication, shared by vertex welding and edge matching.
#pragma once

#include "base/parallel.h"
#include "base/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <vector>

namespace geo::import {

/// Assigns each of `count` items an id, equal items sharing one. Returns the unique count, fills
/// `ids[item]`, and fills `firstItem[id]` with one representative of each id.
///
/// Items are partitioned by the top bits of their hash, so each partition can be processed
/// independently and without locks. The result does not depend on the number of threads.
template <typename Hash, typename Equal>
u32 dedupe(usize count, Hash&& hash, Equal&& equal, std::vector<u32>& ids,
           std::vector<u32>& firstItem) {
    constexpr usize kPartitionBits = 8;
    constexpr usize kPartitions = usize(1) << kPartitionBits;
    constexpr usize kGrain = 1 << 16;
    constexpr u32 kEmpty = 0xFFFFFFFF;
    const auto partitionOf = [](u64 h) { return usize(h >> (64 - kPartitionBits)); };

    std::vector<u64> hashes(count);
    parallelRanges(count, kGrain, [&](usize first, usize last, usize) {
        for (usize i = first; i < last; ++i) {
            hashes[i] = hash(i);
        }
    });

    // Count per partition, then place members in item order so the output is deterministic.
    std::array<usize, kPartitions + 1> offsets{};
    for (usize i = 0; i < count; ++i) {
        ++offsets[partitionOf(hashes[i]) + 1];
    }
    for (usize p = 0; p < kPartitions; ++p) {
        offsets[p + 1] += offsets[p];
    }
    std::vector<u32> members(count);
    std::array<usize, kPartitions> cursors{};
    std::copy_n(offsets.begin(), kPartitions, cursors.begin());
    for (usize i = 0; i < count; ++i) {
        members[cursors[partitionOf(hashes[i])]++] = u32(i);
    }

    // Each partition assigns ids from zero, and its base is added afterwards.
    ids.resize(count);
    std::vector<std::vector<u32>> uniques(kPartitions);
    parallelFor(kPartitions, [&](usize p) {
        const usize size = offsets[p + 1] - offsets[p];
        if (size == 0) {
            return;
        }
        const usize capacity = std::bit_ceil(size * 2);
        std::vector<u32> table(capacity, kEmpty);
        std::vector<u32>& unique = uniques[p];
        for (usize m = offsets[p]; m < offsets[p + 1]; ++m) {
            const u32 item = members[m];
            const u64 h = hashes[item];
            usize slot = usize(h) & (capacity - 1);
            while (true) {
                const u32 existing = table[slot];
                if (existing == kEmpty) {
                    table[slot] = u32(unique.size());
                    ids[item] = u32(unique.size());
                    unique.push_back(item);
                    break;
                }
                const u32 candidate = unique[existing];
                if (hashes[candidate] == h && equal(candidate, item)) {
                    ids[item] = existing;
                    break;
                }
                slot = (slot + 1) & (capacity - 1);
            }
        }
    });

    std::array<u32, kPartitions + 1> bases{};
    for (usize p = 0; p < kPartitions; ++p) {
        bases[p + 1] = bases[p] + u32(uniques[p].size());
    }
    const u32 total = bases[kPartitions];
    firstItem.resize(total);
    parallelFor(kPartitions, [&](usize p) {
        const u32 base = bases[p];
        for (usize m = offsets[p]; m < offsets[p + 1]; ++m) {
            ids[members[m]] += base;
        }
        std::copy(uniques[p].begin(), uniques[p].end(), firstItem.begin() + base);
    });
    return total;
}

/// Mixes a 64-bit value. Input bit patterns such as float coordinates are far from uniform.
constexpr u64 mixHash(u64 value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return value;
}

} // namespace geo::import
