// Which parts are the outer shell? See-through fades the parts that wrap the mechanism (housing,
// covers, caps, and the stator around a rotor) and keeps the inside solid. After step_viewer's
// classification.
//
// From 14 directions the nearest part per pixel is rendered and, peeled, the nearest other part
// behind it. A part is shell when it is mostly seen from outside (exposure), what it hides is
// inner (parts themselves mostly hidden), it does a real share of that hiding, and it encloses at
// least one part it hides. Parts named as the working mechanism (coils, magnets, gears...) never
// fade. The test runs twice: after the first shell is removed, a second layer that hides the
// remaining inner parts decisively fades too. It runs for each separate assembly, then for each
// level of sub-assemblies inside them, each on its own.
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
    Aabb bounds;              // in world space
    u64 definition = ~u64(0); // which shape it is an instance of; copies are decided alike
};

/// The unit directions the views look along: the six axes and the eight diagonals.
std::vector<Vec3> shellViewDirections();

/// Separate assemblies, which see-through classifies each on its own, as it would alone: the parts
/// whose boxes, each grown by 1% of its own diagonal, touch or overlap, directly or through other
/// parts. Returns each part's group, numbered from 0 in order of first appearance.
std::vector<u32> clusterParts(std::span<const Aabb> boxes);

/// One row of an assembly tree, as `nestedGroups` reads it. Rows are depth first, so a row's
/// subtree is the rows from it up to `subtreeEnd`, and its occurrences, in global order, are
/// [`first`, `end`).
struct AssemblyRow {
    u32 first = 0;
    u32 end = 0;
    u32 subtreeEnd = 0;
    bool assembly = false;
};

/// The assemblies one level inside the groups see-through has classified: under each group, the
/// largest assemblies of two or more parts that lie wholly in it and are smaller than it. Each is
/// then classified on its own, as it would be alone, so a motor bolted to a plate is judged as a
/// motor and not beside the whole machine. `groupOf` and the result give each occurrence's group,
/// numbered from 1, or 0 for none; the result's are numbered in tree order.
std::vector<u32> nestedGroups(std::span<const AssemblyRow> rows, std::span<const u32> groupOf);

/// What the test measured of one part, for tracing and tests.
struct ShellScore {
    double exposure = 0.0;  // of its pixels, the share seen from outside
    double occlusion = 0.0; // of its visible pixels, the share in front of inner parts
    double share = 0.0;     // its share of all visible pixels
    double hiding = 0.0;    // its share of all hiding of inner parts
    bool wraps = false;     // it hides an inner part its bounds enclose
    bool shell = false;
};

/// Decides which parts are shell from one round's counts. `minOcclusion` is how much of a part's
/// visible area must hide inner parts. Returns one flag per part, and fills `scores` if given.
std::vector<u8> decideShell(const ShellCounts& counts, std::span<const ShellPart> parts,
                            double minOcclusion, std::vector<ShellScore>* scores = nullptr);

/// Whether a name says a part is the working mechanism, which see-through never fades.
bool isMechanismName(std::string_view name);
/// Whether a name says a part is a housing, cover or the like, which fades on weaker evidence.
bool isShellName(std::string_view name);

} // namespace geo::render
