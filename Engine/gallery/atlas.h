// Atlas: the folder as a map you zoom into, drawn through a camera (atlas.cpp).
#pragma once
#include "gallery/display.h"
#include "gallery/plates.h"

namespace geo::gallery {

/// Where the map is seen from: view points = map units × k + (x, y). `fit` is the k that shows the
/// whole map.
struct AtlasCamera {
    float k = 1.0f, x = 0.0f, y = 0.0f, fit = 1.0f;
};

/// The camera that shows the whole map in a view `width` × `height`.
AtlasCamera atlasFit(float width, float height);
/// The level of detail at a camera: 0 regions, 1 districts, 2 every part.
int atlasLevel(const AtlasCamera& camera);

LayoutResult layoutAtlas(const LayoutInput& input, Display& display, const AtlasCamera& camera);

} // namespace geo::gallery
