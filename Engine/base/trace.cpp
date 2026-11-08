#include "base/trace.h"

#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace geo {
namespace {

// The process's memory footprint now and at its peak so far, in MB, where the system says.
bool footprint(double& now, double& peak) {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &count) != KERN_SUCCESS) {
        return false;
    }
    now = double(info.phys_footprint) / 1048576.0;
    peak = double(info.ledger_phys_footprint_peak) / 1048576.0;
    return true;
#else
    (void)now;
    (void)peak;
    return false;
#endif
}

} // namespace

bool traceEnabled() {
    static const bool enabled = std::getenv("GEOMETRY_TRACE") != nullptr;
    return enabled;
}

void StageTrace::lap(const char* stage) {
    const double now = nowSeconds();
    if (traceEnabled()) {
        double footprintNow = 0.0, footprintPeak = 0.0;
        if (footprint(footprintNow, footprintPeak)) {
            std::fprintf(stderr, "trace: %-10s %-22s %8.1f ms  %6.0f MB, peak %6.0f MB\n", scope_,
                         stage, (now - start_) * 1e3, footprintNow, footprintPeak);
        } else {
            std::fprintf(stderr, "trace: %-10s %-22s %8.1f ms\n", scope_, stage,
                         (now - start_) * 1e3);
        }
    }
    start_ = now;
}

} // namespace geo
