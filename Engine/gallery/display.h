// The display list the gallery's layouts write: rectangles, lines, text and pictures, in order,
// with every colour resolved for the drawing style. Painters draw it as it is.
#pragma once

#include "base/types.h"
#include "gallery.h"
#include "gallery/text.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace geo::gallery {

/// Straight sRGB with alpha, red in the low byte.
constexpr u32 rgba(u32 r, u32 g, u32 b, float a = 1.0f) {
    return r | (g << 8) | (b << 16) | (u32(a * 255.0f + 0.5f) << 24);
}
constexpr u32 hex(u32 rgb, float a = 1.0f) {
    return rgba((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

/// A drawing style's colours on its sheet (Docs/design/gallery-spec.md, "Colour on the sheet").
struct Palette {
    u32 on, muted, faint, rule;
    u32 lift, liftLine, band;
    u32 wash, washLine;         // what a search found, or the selection
    std::array<u32, 4> folders; // mustard, mint, tomato, blush
    bool dark = false;
};
Palette paletteOf(u32 style);

class Display {
public:
    void clear();

    void rect(float x, float y, float w, float h, u32 fill, u32 stroke = 0, float lineWidth = 1.5f,
              float radius = 0.0f, u32 flags = 0, u32 shadow = 0);
    void line(float x0, float y0, float x1, float y1, u32 stroke, float width = 1.0f,
              u32 flags = 0);
    /// One line of text standing on `baseline`, in the box from x to x + width, aligned in it.
    /// Returns the width the text takes.
    float text(std::string_view text, const TextStyle& style, u32 colour, float x, float baseline,
               float width, u32 align = 0);
    /// A file's picture, drawn whole into the rectangle.
    void picture(u32 file, float x, float y, float w, float h);

    /// What a click on the items added from now on means, until it is reset.
    void target(u32 kind, u32 index) {
        targetKind_ = kind;
        target_ = index;
    }
    /// The key the items added from now on carry, and the group they align in.
    void key(u32 value) { key_ = value; }
    void group(u32 value) { group_ = value; }
    /// Items added from now on are moved by this much: a plate lifted under the pointer.
    void offset(float dx, float dy) {
        dx_ = dx;
        dy_ = dy;
    }

    const std::vector<GeoGalleryItem>& items() const { return items_; }
    /// The item added last, or one by index, to say which line or column it aligns on.
    GeoGalleryItem& last() { return items_.back(); }
    GeoGalleryItem& item(usize index) { return items_[index]; }
    /// Puts the item added last in a column of left edges.
    void column(u32 group) {
        items_.back().column = group;
        items_.back().column_line = items_.back().x;
    }
    const std::string& textBuffer() const { return text_; }

private:
    GeoGalleryItem& add(u32 kind);

    std::vector<GeoGalleryItem> items_;
    std::string text_;
    u32 targetKind_ = GeoTargetNone, target_ = 0;
    u32 key_ = 0, group_ = 0;
    float dx_ = 0.0f, dy_ = 0.0f;
};

/// A key for an item from the parts of what it shows, stable from one layout to the next.
u32 keyOf(std::string_view what, u32 index = 0);

} // namespace geo::gallery
