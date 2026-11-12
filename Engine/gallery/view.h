// A gallery: its folder tree, the folder in view, what is hovered, focused and selected, and the
// display list of the view laid out. The C interface (gallery.h) is a thin layer over it.
#pragma once

#include "gallery/display.h"
#include "gallery/plates.h"
#include "gallery/tree.h"

#include <set>
#include <string>
#include <vector>

namespace geo::gallery {

class Gallery {
public:
    void setEntries(std::string_view title, std::span<const Entry> entries);
    Tree& tree() { return tree_; }
    const Tree& tree() const { return tree_; }

    void setFacts(u32 file, Facts facts);
    void setPicture(u32 file, const Picture& picture);
    void setFailure(u32 file, std::string message);

    void setStyle(u32 style);
    void setViewport(float width, float height);
    void setScroll(float top) { scroll_ = top; }

    bool setFolder(std::string_view path);
    u32 folder() const { return folder_; }
    std::string subtitle() const;

    /// Lays out if anything changed. Returns whether it did.
    bool layout();
    const Display& display() const { return display_; }
    const LayoutResult& result() const { return result_; }

    GeoGalleryHit hit(float x, float y) const;
    bool hover(u32 kind, u32 index);
    GeoGalleryAction click(u32 kind, u32 index, bool twice);
    GeoGalleryAction key(u32 key);
    u32 selected() const { return selected_; }

    std::vector<GeoGalleryWant> wanted() const;

private:
    GeoGalleryAction enter(u32 folder);
    // The place nearest the one at `from` in a direction, or none.
    static std::optional<usize> nearest(const std::vector<LayoutResult::Place>& places, usize from,
                                        int dx, int dy);

    Tree tree_;
    u32 folder_ = 0;
    u32 style_ = 0;
    float width_ = 1080.0f, height_ = 720.0f, scroll_ = 0.0f;
    u32 hoverKind_ = GeoTargetNone, hover_ = 0;
    u32 selected_ = kNone, focused_ = kNone;
    std::set<std::string> expanded_;
    bool dirty_ = true;
    Display display_;
    LayoutResult result_;
};

} // namespace geo::gallery
