#include "gallery/checks.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace geo::gallery {

std::vector<Spread> spreads(std::span<const GeoGalleryItem> items, const float* lines,
                            const float* columns) {
    struct Range {
        float lo = INFINITY, hi = -INFINITY;
        u32 n = 0;
    };
    std::map<std::pair<bool, u32>, Range> groups;
    for (usize i = 0; i < items.size(); ++i) {
        const GeoGalleryItem& item = items[i];
        if (item.group != 0) {
            const float v = lines != nullptr ? lines[i] : item.line;
            if (!std::isnan(v)) {
                Range& r = groups[{false, item.group}];
                r.lo = std::min(r.lo, v);
                r.hi = std::max(r.hi, v);
                ++r.n;
            }
        }
        if (item.column != 0) {
            const float v = columns != nullptr ? columns[i] : item.column_line;
            if (!std::isnan(v)) {
                Range& r = groups[{true, item.column}];
                r.lo = std::min(r.lo, v);
                r.hi = std::max(r.hi, v);
                ++r.n;
            }
        }
    }
    std::vector<Spread> out;
    for (const auto& [key, r] : groups) {
        if (r.n >= 2) {
            out.push_back({key.second, key.first, r.hi - r.lo, r.n});
        }
    }
    return out;
}

} // namespace geo::gallery
