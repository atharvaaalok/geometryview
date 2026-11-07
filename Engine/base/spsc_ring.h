// A bounded, lock-free, single-producer single-consumer ring.
//
// The main thread produces and the engine thread consumes, so posting input never takes a lock and
// can never block the UI. Elements must be trivially copyable, so nothing runs on either thread
// while an element moves.
#pragma once

#include "base/types.h"

#include <array>
#include <atomic>
#include <bit>
#include <new>
#include <optional>
#include <type_traits>

namespace geo {

template <typename T, usize Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");

public:
    /// Returns false if the ring is full, leaving the element unsent.
    bool tryPush(const T& value) {
        const usize head = head_.load(std::memory_order_relaxed);
        if (head - tailCache_ == Capacity) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (head - tailCache_ == Capacity) {
                return false;
            }
        }
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> tryPop() {
        const usize tail = tail_.load(std::memory_order_relaxed);
        if (tail == headCache_) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (tail == headCache_) {
                return std::nullopt;
            }
        }
        T value = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }

private:
    // Producer and consumer state sit on separate cache lines to avoid false sharing. Each side
    // caches the other's index, so it touches the shared line only when it appears full or empty.
    alignas(std::hardware_destructive_interference_size) std::atomic<usize> head_{0};
    usize tailCache_ = 0;
    alignas(std::hardware_destructive_interference_size) std::atomic<usize> tail_{0};
    usize headCache_ = 0;
    alignas(std::hardware_destructive_interference_size) std::array<T, Capacity> slots_{};
};

} // namespace geo
