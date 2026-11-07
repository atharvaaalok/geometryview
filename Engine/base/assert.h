// Invariant checks that stay on in release builds. The engine has no exceptions, so a broken
// invariant reports where it happened and stops.
#pragma once

namespace geo {

[[noreturn]] void assertionFailed(const char* condition, const char* file, int line);

} // namespace geo

#define GEO_ASSERT(condition)                                                                      \
    do {                                                                                           \
        if (!(condition)) [[unlikely]] {                                                           \
            ::geo::assertionFailed(#condition, __FILE__, __LINE__);                                \
        }                                                                                          \
    } while (false)
