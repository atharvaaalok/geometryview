#include "base/time.h"

#include <mach/mach_time.h>

namespace geo {

double nowSeconds() {
    static const double ticksToSeconds = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return double(info.numer) / double(info.denom) * 1e-9;
    }();
    return double(mach_absolute_time()) * ticksToSeconds;
}

} // namespace geo
