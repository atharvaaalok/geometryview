#include "base/trace.h"

#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#endif

namespace geo {
namespace {

// The process's memory footprint now and at its peak so far, and the heap's bytes in use, in MB,
// where the system says. The footprint also counts memory freed but kept by the allocator. In
// WebAssembly the footprint is the heap's size.
bool footprint(double& now, double& peak, double& inUse) {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &count) != KERN_SUCCESS) {
        return false;
    }
    malloc_statistics_t heap{};
    malloc_zone_statistics(nullptr, &heap);
    now = double(info.phys_footprint) / 1048576.0;
    peak = double(info.ledger_phys_footprint_peak) / 1048576.0;
    inUse = double(heap.size_in_use) / 1048576.0;
    return true;
#elif defined(__EMSCRIPTEN__)
    // WebAssembly memory never shrinks, so the heap's size is its peak so far. What is in use is
    // not known, since the threaded build's allocator does not say.
    now = peak = double(emscripten_get_heap_size()) / 1048576.0;
    inUse = -1.0;
    return true;
#else
    (void)now;
    (void)peak;
    (void)inUse;
    return false;
#endif
}

} // namespace

bool traceEnabled() {
    static const bool enabled = std::getenv("GEOMETRYVIEW_TRACE") != nullptr;
    return enabled;
}

void StageTrace::lap(const char* stage) {
    const double now = nowSeconds();
    if (traceEnabled()) {
        double footprintNow = 0.0, footprintPeak = 0.0, inUse = 0.0;
        const double ms = (now - start_) * 1e3;
        if (!footprint(footprintNow, footprintPeak, inUse)) {
            std::fprintf(stderr, "trace: %-10s %-22s %8.1f ms\n", scope_, stage, ms);
        } else if (inUse < 0.0) {
            std::fprintf(stderr, "trace: %-10s %-22s %8.1f ms  heap %6.0f MB\n", scope_, stage, ms,
                         footprintNow);
        } else {
            std::fprintf(stderr,
                         "trace: %-10s %-22s %8.1f ms  %6.0f MB, peak %6.0f MB, in use %6.0f MB\n",
                         scope_, stage, ms, footprintNow, footprintPeak, inUse);
        }
    }
    start_ = now;
}

} // namespace geo
