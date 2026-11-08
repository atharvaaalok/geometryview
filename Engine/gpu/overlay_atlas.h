// The overlay's text: the view cube's face labels and the scale bar's glyphs, drawn once.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include "base/types.h"

#include <vector>

namespace geo::gpu {

struct OverlayAtlas {
    id<MTLTexture> texture;
    float glyphAdvance = 0.0f; // a glyph's advance, in atlas pixels
};

/// Draws the atlas's coverage, one byte per pixel, kAtlasWidth by kAtlasHeight, and a glyph's
/// advance in atlas pixels. The bundled IBM Plex Mono is used when the app has registered it, and
/// Menlo otherwise.
bool drawOverlayAtlas(std::vector<u8>& pixels, float& glyphAdvance);

/// Draws the atlas into one small texture.
bool makeOverlayAtlas(id<MTLDevice> device, OverlayAtlas& atlas);

} // namespace geo::gpu
#endif
