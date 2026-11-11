// The gallery's C interface, for Swift and for WebAssembly.
#include "gallery.h"

#include "gallery/checks.h"
#include "gallery/view.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace geo;

struct GeoGallery {
    gallery::Gallery gallery;
    std::string folderPath;
    std::string title;
    std::string subtitle;
};

GeoGallery* geo_gallery_create(void) {
    return new GeoGallery();
}

void geo_gallery_destroy(GeoGallery* gallery) {
    delete gallery;
}

void geo_gallery_set_entries(GeoGallery* gallery, const char* title, const GeoGalleryEntry* entries,
                             uint32_t count) {
    std::vector<gallery::Entry> list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].path != nullptr) {
            list.push_back(
                {entries[i].path, entries[i].folder, entries[i].bytes, entries[i].modified});
        }
    }
    gallery->gallery.setEntries(title != nullptr ? title : "", list);
}

uint32_t geo_gallery_file_count(const GeoGallery* gallery) {
    return gallery->gallery.tree().fileCount();
}

const char* geo_gallery_file_path(const GeoGallery* gallery, uint32_t file) {
    return file < gallery->gallery.tree().fileCount()
               ? gallery->gallery.tree().file(file).path.c_str()
               : "";
}

uint32_t geo_gallery_file_at(const GeoGallery* gallery, const char* path) {
    return path != nullptr ? gallery->gallery.tree().fileAt(path) : gallery::kNone;
}

void geo_gallery_set_facts(GeoGallery* gallery, uint32_t file, const GeoGalleryFacts* facts) {
    if (file >= gallery->gallery.tree().fileCount() || facts == nullptr) {
        return;
    }
    gallery::Facts f;
    f.size = {facts->size[0], facts->size[1], facts->size[2]};
    f.hasBox = facts->has_box;
    f.estimated = facts->estimated;
    f.parts = facts->parts;
    f.product = facts->product != nullptr ? facts->product : "";
    gallery->gallery.setFacts(file, std::move(f));
}

void geo_gallery_set_picture(GeoGallery* gallery, uint32_t file, const GeoGalleryPicture* picture) {
    if (file >= gallery->gallery.tree().fileCount() || picture == nullptr) {
        return;
    }
    gallery::Picture p;
    p.width = picture->width;
    p.height = picture->height;
    p.mmPerPixel = picture->mm_per_pixel;
    std::copy(picture->part, picture->part + 4, p.part.begin());
    std::copy(picture->ink, picture->ink + 4, p.ink.begin());
    gallery->gallery.setPicture(file, p);
}

void geo_gallery_set_failure(GeoGallery* gallery, uint32_t file, const char* message) {
    if (file < gallery->gallery.tree().fileCount()) {
        gallery->gallery.setFailure(file, message != nullptr && *message != '\0'
                                              ? message
                                              : "This file could not be read.");
    }
}

void geo_gallery_set_style(GeoGallery* gallery, uint32_t style) {
    gallery->gallery.setStyle(style);
}

void geo_gallery_set_viewport(GeoGallery* gallery, float width, float height) {
    gallery->gallery.setViewport(std::max(width, 120.0f), std::max(height, 120.0f));
}

void geo_gallery_set_scroll(GeoGallery* gallery, float top) {
    gallery->gallery.setScroll(top);
}

bool geo_gallery_set_folder(GeoGallery* gallery, const char* path) {
    return gallery->gallery.setFolder(path != nullptr ? path : "");
}

const char* geo_gallery_folder(const GeoGallery* gallery) {
    auto* g = const_cast<GeoGallery*>(gallery);
    g->folderPath = gallery->gallery.tree().folder(gallery->gallery.folder()).path;
    return g->folderPath.c_str();
}

const char* geo_gallery_folder_title(const GeoGallery* gallery) {
    auto* g = const_cast<GeoGallery*>(gallery);
    g->title = gallery->gallery.tree().titleOf(gallery->gallery.folder());
    return g->title.c_str();
}

const char* geo_gallery_folder_subtitle(const GeoGallery* gallery) {
    auto* g = const_cast<GeoGallery*>(gallery);
    g->subtitle = gallery->gallery.subtitle();
    return g->subtitle.c_str();
}

bool geo_gallery_layout(GeoGallery* gallery) {
    return gallery->gallery.layout();
}

uint32_t geo_gallery_item_count(const GeoGallery* gallery) {
    return uint32_t(gallery->gallery.display().items().size());
}

const GeoGalleryItem* geo_gallery_items(const GeoGallery* gallery) {
    return gallery->gallery.display().items().data();
}

const char* geo_gallery_text(const GeoGallery* gallery) {
    return gallery->gallery.display().textBuffer().c_str();
}

float geo_gallery_content_height(const GeoGallery* gallery) {
    return gallery->gallery.result().contentHeight;
}

uint32_t geo_gallery_picture_pixels(const GeoGallery*) {
    // Plates draw a part at most 0.625 points a pixel, and its cover parts up to about 280 points.
    return 512;
}

GeoGalleryHit geo_gallery_hit(const GeoGallery* gallery, float x, float y) {
    return gallery->gallery.hit(x, y);
}

bool geo_gallery_hover(GeoGallery* gallery, uint32_t target_kind, uint32_t target) {
    return gallery->gallery.hover(target_kind, target);
}

GeoGalleryAction geo_gallery_click(GeoGallery* gallery, uint32_t target_kind, uint32_t target,
                                   bool twice) {
    return gallery->gallery.click(target_kind, target, twice);
}

GeoGalleryAction geo_gallery_key(GeoGallery* gallery, uint32_t key) {
    return gallery->gallery.key(key);
}

uint32_t geo_gallery_selected(const GeoGallery* gallery) {
    return gallery->gallery.selected();
}

uint32_t geo_gallery_wanted(const GeoGallery* gallery, GeoGalleryWant* out, uint32_t capacity) {
    const std::vector<GeoGalleryWant> wanted = gallery->gallery.wanted();
    if (out != nullptr) {
        std::copy_n(wanted.begin(), std::min<usize>(capacity, wanted.size()), out);
    }
    return uint32_t(wanted.size());
}

uint32_t geo_gallery_check(const GeoGallery* gallery, const float* lines, const float* columns,
                           float tolerance, GeoGalleryGroupSpread* out, uint32_t capacity,
                           uint32_t* checked) {
    const std::vector<GeoGalleryItem>& items = gallery->gallery.display().items();
    const std::vector<gallery::Spread> spreads = gallery::spreads(items, lines, columns);
    uint32_t over = 0;
    for (const gallery::Spread& s : spreads) {
        if (s.spread > tolerance) {
            if (out != nullptr && over < capacity) {
                out[over] = {s.group, s.column, s.spread, s.items};
            }
            ++over;
        }
    }
    if (checked != nullptr) {
        *checked = uint32_t(spreads.size());
    }
    return over;
}

void geo_gallery_set_views(GeoGallery* gallery, uint32_t mask) {
    gallery->gallery.setViews(mask);
}

uint32_t geo_gallery_views(const GeoGallery* gallery) {
    return gallery->gallery.views();
}

bool geo_gallery_set_view(GeoGallery* gallery, uint32_t view) {
    return gallery->gallery.setView(view);
}

uint32_t geo_gallery_view(const GeoGallery* gallery) {
    return gallery->gallery.view();
}

void geo_gallery_set_sheet(GeoGallery* gallery, uint32_t scale, float cell, uint32_t sort) {
    gallery->gallery.setSheet(scale, cell, sort);
}

void geo_gallery_set_search(GeoGallery* gallery, const char* query) {
    gallery->gallery.setSearch(query != nullptr ? query : "");
}

const char* geo_gallery_search(const GeoGallery* gallery) {
    return gallery->gallery.search().c_str();
}

uint32_t geo_gallery_matches(const GeoGallery* gallery) {
    const auto& matches = gallery->gallery.matches();
    return matches ? uint32_t(matches->size()) : UINT32_MAX;
}

void geo_gallery_peek(GeoGallery* gallery, uint32_t file) {
    gallery->gallery.peek(file);
}

uint32_t geo_gallery_peeking(const GeoGallery* gallery) {
    return gallery->gallery.peeking();
}

float geo_gallery_reveal(const GeoGallery* gallery) {
    return gallery->gallery.reveal();
}

GeoGalleryTransition geo_gallery_transition(const GeoGallery* gallery) {
    return gallery->gallery.transition();
}

uint32_t geo_gallery_tracks(const GeoGallery* gallery, GeoGalleryTrack* out, uint32_t capacity) {
    const std::vector<GeoGalleryTrack>& tracks = gallery->gallery.tracks();
    if (out != nullptr) {
        std::copy_n(tracks.begin(), std::min<usize>(capacity, tracks.size()), out);
    }
    return uint32_t(tracks.size());
}
