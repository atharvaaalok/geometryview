// Counts heap allocations on the calling thread, to enforce zero allocations per frame.
//
// A dropped frame costs far more than the allocation itself, so a frame must not touch the heap.
// Metal and Core Animation allocate inside their own calls (about 52 a frame on macOS 26: encoders,
// and the drawable's wait), which no Metal program avoids, so those are counted apart.
// This hooks the malloc logger, which exists for developer tools, and is enabled by
// GEOMETRY_ALLOCS=1 only. GEOMETRY_ALLOCS=stacks also keeps each counted allocation's call stack,
// so one frame's allocations can be printed and traced to their source.
#pragma once

#include "base/types.h"

namespace geo::engine {

bool allocationCountingEnabled();

/// Installs the hook. Counts only on the thread that calls this.
void startCountingAllocations();

/// Removes the hook, for tests that count on a thread of their own.
void stopCountingAllocations();

u64 allocationCount();

/// Of those, the ones made by this program's own code, rather than inside the GPU driver or the
/// display's frameworks on its behalf. These are the ones a frame must not make.
u64 ownAllocationCount();

/// With GEOMETRY_ALLOCS=stacks: from now on, keeps the call stacks of counted allocations (up to a
/// fixed number), forgetting those kept before.
void keepAllocationStacks();

/// Writes the kept call stacks to `fd`, symbolized, without allocating.
void printAllocationStacks(int fd);

} // namespace geo::engine
