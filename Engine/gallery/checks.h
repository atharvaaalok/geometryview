// The alignment check: every group of like items stands on one line, within half a point, as
// painted. Docs/design/gallery-spec.md, "Alignment".
#pragma once

#include "base/types.h"
#include "gallery.h"

#include <span>
#include <vector>

namespace geo::gallery {

struct Spread {
    u32 group = 0;
    bool column = false;
    float spread = 0.0f;
    u32 items = 0;
};

/// Each group's spread: for rows, over `lines[i]` (where item i's line was painted); for columns,
/// over `columns[i]`. Null measurements check the layout's own lines; NaN leaves an item out.
std::vector<Spread> spreads(std::span<const GeoGalleryItem> items, const float* lines,
                            const float* columns);

} // namespace geo::gallery
