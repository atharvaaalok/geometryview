// Data parallelism on the system thread pool.
#pragma once

#include "base/types.h"

#include <algorithm>
#include <dispatch/dispatch.h>
#include <thread>
#include <type_traits>

namespace geo {

/// Calls `body(index)` for each index in [0, count), in parallel and in no particular order.
template <typename Body>
void parallelFor(usize count, Body&& body) {
    auto trampoline = [](void* context, size_t index) {
        (*static_cast<std::remove_reference_t<Body>*>(context))(usize(index));
    };
    dispatch_apply_f(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &body, trampoline);
}

/// Splits [0, count) into at most one range per core and calls `body(first, last, rangeIndex)`.
/// Ranges are never smaller than `grain`, so small inputs stay on one thread.
template <typename Body>
void parallelRanges(usize count, usize grain, Body&& body) {
    if (count == 0) {
        return;
    }
    const usize cores = std::max<usize>(1, std::thread::hardware_concurrency());
    const usize ranges = std::clamp<usize>(count / std::max<usize>(grain, 1), 1, cores);
    parallelFor(ranges, [&](usize range) {
        body(count * range / ranges, count * (range + 1) / ranges, range);
    });
}

} // namespace geo
