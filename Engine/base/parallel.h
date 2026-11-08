// Data parallelism: Apple's system thread pool, a pool of our own elsewhere, or a plain loop where
// there are no threads, as in single-threaded WebAssembly.
#pragma once

#include "base/types.h"

#include <algorithm>
#include <type_traits>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define GEO_HAS_THREADS 0
#else
#define GEO_HAS_THREADS 1
#endif

namespace geo {

/// How many threads parallel work may use, the calling one included.
usize parallelWidth();

namespace detail {
/// Calls `function(context, index)` for each index in [0, count) on the shared pool.
void poolFor(usize count, void* context, void (*function)(void*, usize));
} // namespace detail

/// Calls `body(index)` for each index in [0, count), in parallel and in no particular order.
template <typename Body>
void parallelFor(usize count, Body&& body) {
#if defined(__APPLE__) || GEO_HAS_THREADS
    auto trampoline = [](void* context, size_t index) {
        (*static_cast<std::remove_reference_t<Body>*>(context))(usize(index));
    };
#endif
#if defined(__APPLE__)
    dispatch_apply_f(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &body,
                     trampoline);
#elif GEO_HAS_THREADS
    detail::poolFor(count, &body, trampoline);
#else
    for (usize index = 0; index < count; ++index) {
        body(index);
    }
#endif
}

/// The number of ranges `parallelRanges` uses for these arguments, for sizing per-range state.
inline usize parallelRangeCount(usize count, usize grain) {
    return std::clamp<usize>(count / std::max<usize>(grain, 1), 1, parallelWidth());
}

/// Splits [0, count) into at most one range per core and calls `body(first, last, rangeIndex)`.
/// Ranges are never smaller than `grain`, so small inputs stay on one thread.
template <typename Body>
void parallelRanges(usize count, usize grain, Body&& body) {
    if (count == 0) {
        return;
    }
    const usize ranges = parallelRangeCount(count, grain);
    parallelFor(ranges, [&](usize range) {
        // In 64 bits, since the products overflow a 32-bit usize, as in WebAssembly.
        body(usize(u64(count) * range / ranges), usize(u64(count) * (range + 1) / ranges), range);
    });
}

/// Runs one task alongside the caller, who collects it with `wait`. Where there are no threads,
/// the task runs inside `wait`.
class Concurrent {
public:
    Concurrent() = default;
    Concurrent(const Concurrent&) = delete;
    Concurrent& operator=(const Concurrent&) = delete;
    ~Concurrent() { wait(); }

    void start(void* context, void (*function)(void*));
    void wait();

private:
    void* context_ = nullptr;
    void (*function_)(void*) = nullptr;
#if defined(__APPLE__)
    dispatch_group_t group_ = nullptr;
#elif GEO_HAS_THREADS
    void* thread_ = nullptr; // a std::thread, kept out of this header
#endif
};

} // namespace geo
