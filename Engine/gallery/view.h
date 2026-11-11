// A gallery: its folder tree, the folder in view, what is hovered, focused and selected, and the
// display list of the view laid out. The C interface (gallery.h) is a thin layer over it.
#pragma once

#include "gallery/atlas.h"
#include "gallery/catalogue.h"
#include "gallery/display.h"
#include "gallery/plates.h"
#include "gallery/tree.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    void setReading(u32 file, float fraction);
    void setPoster(bool poster, std::string date);
    void setLost(bool lost);
    void setLaneScroll(u32 lane, float x) {
        laneScroll_[lane] = x;
        // A comparison's card is placed over the hovered part as its lane is scrolled.
        if (view_ == GeoViewLadders && selected_ != kNone && hoverKind_ == GeoTargetFile) {
            dirty_ = true;
        }
    }
    /// Atlas's camera: fitted to the view until moved, and again for a new folder or view size.
    AtlasCamera atlasCamera() const {
        return atlas_ && atlasFor_ == std::pair{width_, height_} ? *atlas_
                                                                 : atlasFit(width_, height_);
    }
    void setAtlasCamera(float k, float x, float y) {
        AtlasCamera cam = atlasFit(width_, height_);
        cam.k = std::clamp(k, cam.fit * 0.6f, cam.fit * 12.0f);
        cam.x = x;
        cam.y = y;
        atlas_ = cam;
        atlasFor_ = {width_, height_};
        dirty_ = true;
    }
    void resetAtlasCamera() {
        atlas_.reset();
        dirty_ = true;
    }
    /// Catalogue's spread in view, how many the book has, and the index's.
    u32 catalogueSpread() const { return spread_; }
    u32 catalogueSpreads() const { return ::geo::gallery::catalogueSpreads(tree_, folder_); }
    u32 catalogueIndexSpread() const {
        return ::geo::gallery::catalogueIndexSpread(tree_, folder_);
    }
    void setCatalogueSpread(u32 spread) {
        const u32 last = std::max(1u, catalogueSpreads()) - 1;
        if (std::min(spread, last) != spread_) {
            spread_ = std::min(spread, last);
            selected_ = kNone;
            dirty_ = true;
        }
    }
    float viewWidth() const { return width_; }
    float viewHeight() const { return height_; }
    void setLadders(bool shared) {
        if (shared != laddersShared_) {
            laddersShared_ = shared;
            dirty_ = true;
        }
    }
    const GeoGalleryLane* laneOf(u32 lane) const {
        for (const GeoGalleryLane& l : result_.lanes) {
            if (l.id == lane) {
                return &l;
            }
        }
        return nullptr;
    }
    float laneScroll(u32 lane) const {
        const auto it = laneScroll_.find(lane);
        return it == laneScroll_.end() ? 0.0f : it->second;
    }
    bool poster() const { return poster_; }
    void setChanged(u32 file, bool changed);

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

    // ---- Views and what they share ----

    void setViews(u32 mask);
    u32 views() const { return views_; }
    bool setView(u32 view);
    u32 view() const { return view_; }
    void setSheet(u32 scale, float cell, u32 sort);
    void setSearch(std::string_view query);
    const std::string& search() const { return search_; }
    /// Files in the folder the search found, or nothing with no search.
    const std::optional<std::unordered_set<u32>>& matches() const { return matches_; }
    void peek(u32 file);
    u32 peeking() const { return peek_; }
    /// Where to scroll so the selection shows, or negative if it shows already.
    float reveal() const;
    /// The selection's lane and the scroll that shows it, if it is in a lane and does not show.
    std::optional<std::pair<u32, float>> revealLane() const;
    const GeoGalleryTransition& transition() const { return transition_; }
    const std::vector<GeoGalleryTrack>& tracks() const { return tracks_; }

private:
    GeoGalleryAction enter(u32 folder);
    /// Remembers where pictures were in the view, before a change that moves them.
    void beginMotion(u32 kind, std::optional<std::array<float, 4>> rect = std::nullopt);
    void findMatches();
    void layoutPeek();
    /// A changed file was looked at: its Δ goes.
    void seen(u32 file);
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
    u32 views_ = (1u << GeoViewPlates) | (1u << GeoViewSheet);
    u32 view_ = GeoViewPlates;
    std::unordered_map<u32, u32> viewOf_; // the view each folder last showed
    u32 sheetScale_ = GeoScaleFolder;
    float sheetCell_ = 128.0f;
    u32 sort_ = GeoSortName;
    std::string search_;
    std::optional<std::unordered_set<u32>> matches_;
    u32 peek_ = kNone;
    bool poster_ = false;
    bool lost_ = false;
    bool laddersShared_ = false;
    std::optional<AtlasCamera> atlas_;
    u32 spread_ = 0;
    std::pair<float, float> atlasFor_;
    std::unordered_map<u32, float> laneScroll_;
    void layoutLost();
    std::string posterDate_;
    // Motion: what the next layout should play, and what the last one did.
    u32 pendingMotion_ = GeoTransitionNone;
    std::optional<std::array<float, 4>> pendingRect_;
    std::unordered_map<u32, std::array<float, 4>> before_; // a file's picture, in the view
    GeoGalleryTransition transition_{};
    std::vector<GeoGalleryTrack> tracks_;
    bool dirty_ = true;
    Display display_;
    LayoutResult result_;
};

} // namespace geo::gallery
