#include "base/time.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <chrono>
#endif

namespace geo {

double nowSeconds() {
#if defined(__APPLE__)
    static const double ticksToSeconds = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return double(info.numer) / double(info.denom) * 1e-9;
    }();
    return double(mach_absolute_time()) * ticksToSeconds;
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

} // namespace geo
