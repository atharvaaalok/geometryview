// Stage timing for performance work. Prints only when GEOMETRY_TRACE is set, so it stays in the
// code and costs one branch otherwise.
#pragma once

#include "base/time.h"

namespace geo {

bool traceEnabled();

/// Prints the seconds since construction or the previous `lap`, labelled.
class StageTrace {
public:
    explicit StageTrace(const char* scope) : scope_(scope), start_(nowSeconds()) {}
    void lap(const char* stage);

private:
    const char* scope_;
    double start_;
};

} // namespace geo
