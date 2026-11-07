#include "base/assert.h"

#include <cstdio>
#include <cstdlib>

namespace geo {

void assertionFailed(const char* condition, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, condition);
    std::abort();
}

} // namespace geo
