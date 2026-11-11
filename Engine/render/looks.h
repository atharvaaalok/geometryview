// Colour modes: what each body looks like, decided once per mode and shared by every style.
#pragma once

#include "base/types.h"
#include "render/style.h"
#include "scene/scene.h"

#include <optional>
#include <span>
#include <vector>

namespace geo::render {

inline constexpr u32 kMaxLooks = kLookIndex; // the top bit marks the shell

/// The distinct looks of a scene, and which one each occurrence has.
struct LookAssignment {
    std::vector<Look> looks;
    std::vector<u16> ofOccurrence; // in global occurrence order
};

/// Material mode reads names, a body's and then its part's, and otherwise cycles colours per
/// top-level sub-assembly, so neighbouring groups differ. Parts mode cycles per part. File mode
/// uses stored colours, and mono gives every body the same neutral look.
///
/// Cycle numbers count across documents, so two files shown together differ, except for a
/// document given a first colour in `firstColors` (indexed by document): its numbering starts at
/// that place in the cycle and leaves the shared count alone. The gallery uses it to give a file
/// its folder's colour.
LookAssignment assignLooks(const scene::Scene& scene, ColorMode mode,
                           std::span<const std::optional<u32>> firstColors = {});

/// Three palette texels for each look under a style, in look order.
std::vector<u32> paletteTexels(const Style& style, std::span<const Look> looks);

/// Each occurrence's swatch colour under a style, RGBA8, in global occurrence order.
std::vector<u32> swatchColors(const Style& style, const LookAssignment& assignment);

} // namespace geo::render
