// What every view draws with: a file's size as drawn, the order of sizes, names for a range of
// files, and the drawing of a part, a scale bar and their alignment, onto a display list.
#pragma once

#include "gallery/display.h"
#include "gallery/plates.h"
#include "gallery/tree.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace geo::gallery {

inline constexpr TextStyle kBarNumber{10.0f, 600, 0.04f, false};
inline constexpr TextStyle kBarNumberSmall{9.0f, 600, 0.04f, false};
inline constexpr TextStyle kLabel{10.5f, 400, 0.0f, false};
inline constexpr TextStyle kLabelSub{9.5f, 400, 0.0f, false};
inline constexpr TextStyle kMore{11.5f, 400, 0.0f, false};

/// A file's part as drawn, in millimetres, and its ink past the part on each side: its shadow.
struct Drawn {
    double w = 0, h = 0;
    double below = 0;
    double left = 0, right = 0, above = 0;
};

/// The part's size, or nothing for a file that failed or is not measured yet.
std::optional<Drawn> drawnOf(const File& file);
/// The files of `files` with a size to draw.
std::vector<u32> drawable(const Tree& tree, const std::vector<u32>& files);
double largest(const Drawn& d);
/// Files by their largest side, then by name.
std::vector<u32> bySize(const Tree& tree, std::vector<u32> files);
/// "M1–M36" when the names read as one series, else the span of sizes.
std::string rangeOf(const Tree& tree, const std::vector<u32>& files);

/// Draws onto a display list for one layout.
class Draw {
public:
    Draw(const LayoutInput& input, Display& display, LayoutResult& result)
        : in(input), tree(*input.tree), out(display), result(result),
          palette(paletteOf(input.style)) {}

    u32 newGroup() { return ++result.groups; }
    /// A file's part, its bottom at `bottom`, its left edge at `x`, at `mmPerPoint`: its picture,
    /// or its measured box in construction lines. Returns its size in points.
    /// With `marks`, a file being read shows its bar and a changed one its Δ.
    std::array<float, 2> part(u32 file, float x, float bottom, double mmPerPoint, u32 group,
                              bool marks = true);
    void lastLine(float line) { out.last().line = line; }
    /// A scale bar: its length above an ink bracket. Returns its width; the number stands on
    /// `baseline`, centred in it.
    float scaleBar(double mmPerPoint, float x, float baseline, u32 colour, u32 group,
                   bool small = false);
    static float scaleBarWidth(double mmPerPoint, bool small = false);

    /// A poster's printed head: the folder's title large, what it holds, and on the right
    /// `eyebrow` and the drawing style. Returns where the view starts below it.
    float posterHead(std::string_view summary, std::string_view eyebrow, float width);
    /// Its foot at `y`: a rule, then the folder's path, GeometryView and the style, and the date.
    /// Returns the poster's bottom.
    float posterFoot(float y, float width);

    const LayoutInput& in;
    const Tree& tree;
    Display& out;
    LayoutResult& result;
    Palette palette;
};

} // namespace geo::gallery
