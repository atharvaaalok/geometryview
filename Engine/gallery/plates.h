// Plates: a folder's subfolders as plates, all on one screen, each with a cover of its parts at one
// scale; and the folder's own files standing on a shelf at one shared scale, smallest to largest.
// Docs/design/gallery-spec.md, "Views".
#pragma once

#include "gallery/display.h"
#include "gallery/tree.h"

#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace geo::gallery {

/// What a layout is asked to show.
struct LayoutInput {
    const Tree* tree = nullptr;
    u32 folder = 0;
    float width = 1080.0f, height = 720.0f; // the view, in points
    u32 style = 0;
    u32 hoverKind = GeoTargetNone, hover = 0;
    u32 selected = kNone;                            // a file
    u32 focused = kNone;                             // a subfolder, moved to with the arrows
    const std::set<std::string>* expanded = nullptr; // size bands shown whole
    // Sheet's controls.
    u32 sheetScale = GeoScaleFolder;
    float sheetCell = 128.0f;
    u32 sort = GeoSortName;
    /// What a search found in the folder, or null with no search.
    const std::unordered_set<u32>* matches = nullptr;
    /// A poster: the view printed on one sheet, its title above and a footer rule below, with
    /// nothing hovered or selected; `posterDate` is printed in the foot.
    bool poster = false;
    std::string_view posterDate;
    /// Ladders: every strip at one scale.
    bool laddersShared = false;
};

/// What a layout found out, beside its items: where things are, for keys and wanted pictures.
struct LayoutResult {
    float contentHeight = 0.0f;
    struct Place {
        u32 kind, index; // GeoTargetFolder or GeoTargetFile
        float x, y, w, h;
        u32 lane = 0; // placed in a lane's content
    };
    std::vector<Place> plates;      // subfolders, in order
    std::vector<Place> parts;       // files on shelves, in order
    std::vector<Place> covers;      // files drawn on plates' covers
    std::vector<std::string> bands; // keys of size bands cut short, by their "more" target index
    u32 groups = 0;                 // alignment groups made
    std::vector<GeoGalleryLane> lanes; // bands that scroll sideways on their own
};

/// Content insets: one left edge for every view.
inline constexpr float kContentLeft = 6.0f;
inline constexpr float kContentRight = 18.0f;
inline constexpr float kContentTop = 2.0f;
inline constexpr float kContentBottom = 30.0f;

/// Pictures display at most 1.25 times their own pixels at 2 per point: 0.625 points a pixel.
inline constexpr float kMaxPointsPerPixel = 0.625f;

/// Ladders: every folder holding files a strip, smallest to largest, on a ruler at its scale; each
/// strip's parts in a lane of their own. `laneScroll` says how far each lane is scrolled.
LayoutResult layoutLadders(const LayoutInput& input, Display& display,
                           const std::function<float(u32)>& laneScroll);
LayoutResult layoutPlates(const LayoutInput& input, Display& display);
/// Sheet: every file once, folder by folder under headings that stay put while scrolling.
LayoutResult layoutSheet(const LayoutInput& input, Display& display);

/// A group of files at one scale: millimetres per point that fit a typical one in w × h, and those
/// far larger than the rest (more than `over` times the median), drawn apart. With `shadows`, a
/// part's shadow below it must fit too.
struct FamilyScale {
    double mmPerPoint = 1.0;
    std::vector<u32> normal, oversize;
};
FamilyScale familyScale(const Tree& tree, const std::vector<u32>& files, float w, float h,
                        double fraction = 0.8, double over = 3.0, bool shadows = false);

/// A scale bar's length in millimetres: 1, 2 or 5 × 10ⁿ, at least 40 points long.
double scaleBarLength(double mmPerPoint);

} // namespace geo::gallery
