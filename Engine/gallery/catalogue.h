// Catalogue: the folder typeset as a printed parts catalogue, in two-page spreads (catalogue.cpp).
#pragma once
#include "gallery/display.h"
#include "gallery/plates.h"

namespace geo::gallery {

/// How many spreads the folder's book has, and the one its index starts on.
u32 catalogueSpreads(const Tree& tree, u32 folder);
u32 catalogueIndexSpread(const Tree& tree, u32 folder);

/// The spread `spread` of the folder's book, fitted to the view.
LayoutResult layoutCatalogue(const LayoutInput& input, Display& display, u32 spread);

} // namespace geo::gallery
