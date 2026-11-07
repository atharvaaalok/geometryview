// Counts heap allocations on the calling thread, to enforce zero allocations per frame.
//
// A dropped frame costs far more than the allocation itself, so a frame must not touch the heap.
// This hooks the malloc logger, which exists for developer tools, and is enabled by
// GEOMETRY_ALLOCS=1 only.
#pragma once

#include "base/types.h"

namespace geo::engine {

bool allocationCountingEnabled();

/// Installs the hook. Counts only on the thread that calls this.
void startCountingAllocations();

u64 allocationCount();

} // namespace geo::engine
