// Which parts are the outer shell? See-through fades the parts that wrap the mechanism (housing,
// covers, caps, and the stator around a rotor) and keeps the inside solid. After step_viewer's
// classification.
//
// From 14 directions the nearest part per pixel is rendered and, peeled, the nearest other part
// behind it. A part is shell when it is mostly seen from outside (exposure), what it hides is
// inner (parts themselves mostly hidden), it does a real share of that hiding, and it encloses at
// least one part it hides. Parts named as the working mechanism (coils, magnets, gears...) never
// fade. The test runs twice: after the first shell is removed, a second layer that hides the
// remaining inner parts decisively fades too.
#pragma once

#include "base/types.h"
#include "math/geometry.h"

#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geo::render {

/// Pixel counts from the views of one round.
struct ShellCounts {
    explicit ShellCounts(u32 partCount);

    /// Adds one view: per pixel, the nearest part and the nearest other part behind it, as index
    /// plus one, or zero for none.
    void accumulate(std::span<const u32> front, std::span<const u32> behind);

    u32 parts;
    std::vector<double> front;             // pixels where the part is nearest
    std::vector<double> behind;            // pixels where it is the nearest part behind another
    std::unordered_map<u64, double> pairs; // front * parts + behind, to the pixels of that pair
};

struct ShellPart {
    std::string_view name;
    Aabb bounds; // in world space
};

/// The unit directions the views look along: the six axes and the eight diagonals.
std::vector<Vec3> shellViewDirections();

/// Decides which parts are shell from one round's counts. `minOcclusion` is how much of a part's
/// visible area must hide inner parts. Returns one flag per part.
std::vector<u8> decideShell(const ShellCounts& counts, std::span<const ShellPart> parts,
                            double minOcclusion);

/// Whether a name says a part is the working mechanism, which see-through never fades.
bool isMechanismName(std::string_view name);
/// Whether a name says a part is a housing, cover or the like, which fades on weaker evidence.
bool isShellName(std::string_view name);

} // namespace geo::render
