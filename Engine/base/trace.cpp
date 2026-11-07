#include "base/trace.h"

#include <cstdio>
#include <cstdlib>

namespace geo {

bool traceEnabled() {
    static const bool enabled = std::getenv("GEOMETRY_TRACE") != nullptr;
    return enabled;
}

void StageTrace::lap(const char* stage) {
    const double now = nowSeconds();
    if (traceEnabled()) {
        std::fprintf(stderr, "trace: %-10s %-22s %8.1f ms\n", scope_, stage, (now - start_) * 1e3);
    }
    start_ = now;
}

} // namespace geo
