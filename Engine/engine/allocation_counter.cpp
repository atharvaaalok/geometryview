#include "engine/allocation_counter.h"

#include <cstdint>
#include <cstdlib>
#include <pthread.h>

// Declared by the system's stack logging, and not in a public header.
extern "C" {
using MallocLogger = void(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                          uintptr_t result, uint32_t skip);
extern MallocLogger* malloc_logger;
}

namespace geo::engine {
namespace {

constexpr uint32_t kLogTypeAllocate = 2;
// Not thread_local: touching a thread-local for the first time allocates, which would re-enter this
// hook forever.
pthread_t countedThread = nullptr;
u64 allocations = 0;

void logAllocation(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t) {
    if ((type & kLogTypeAllocate) != 0 && pthread_equal(pthread_self(), countedThread) != 0) {
        ++allocations;
    }
}

} // namespace

bool allocationCountingEnabled() {
    static const bool enabled = std::getenv("GEOMETRY_ALLOCS") != nullptr;
    return enabled;
}

void startCountingAllocations() {
    countedThread = pthread_self();
    malloc_logger = logAllocation;
}

u64 allocationCount() {
    return allocations;
}

} // namespace geo::engine
