// Monotonic time on the same clock as NSEvent timestamps and CAMetalDisplayLink, so input and
// presentation times can be subtracted directly.
#pragma once

namespace geo {

double nowSeconds();

} // namespace geo
