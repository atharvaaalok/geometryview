// Colour modes: what each body looks like, decided once per mode and shared by every style.
#pragma once

#include "base/types.h"
#include "render/style.h"
#include "scene/scene.h"

#include <span>
#include <vector>

namespace geo::render {

inline constexpr u32 kMaxLooks = 65535;

/// The distinct looks of a scene, and which one each occurrence has.
struct LookAssignment {
    std::vector<Look> looks;
    std::vector<u16> ofOccurrence; // in global occurrence order
};

/// Material mode reads names, a body's and then its part's, and otherwise cycles colours per
/// top-level sub-assembly, so neighbouring groups differ. Parts mode cycles per part. File mode
/// uses stored colours, and mono gives every body the same neutral look.
LookAssignment assignLooks(const scene::Scene& scene, ColorMode mode);

/// Three palette texels for each look under a style, in look order.
std::vector<u32> paletteTexels(const Style& style, std::span<const Look> looks);

} // namespace geo::render
