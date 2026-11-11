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
    f.triangles = facts->triangles;
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

void geo_gallery_set_poster(GeoGallery* gallery, bool poster, const char* date) {
    gallery->gallery.setPoster(poster, date != nullptr ? date : "");
}

bool geo_gallery_poster(const GeoGallery* gallery) {
    return gallery->gallery.poster();
}

uint32_t geo_gallery_lanes(const GeoGallery* gallery, GeoGalleryLane* out, uint32_t capacity) {
    const std::vector<GeoGalleryLane>& lanes = gallery->gallery.result().lanes;
    if (out != nullptr) {
        std::copy_n(lanes.begin(), std::min<usize>(capacity, lanes.size()), out);
    }
    return uint32_t(lanes.size());
}

void geo_gallery_set_lane_scroll(GeoGallery* gallery, uint32_t lane, float x) {
    gallery->gallery.setLaneScroll(lane, x);
}

bool geo_gallery_reveal_lane(const GeoGallery* gallery, uint32_t* lane, float* x) {
    const std::optional<std::pair<u32, float>> reveal = gallery->gallery.revealLane();
    if (!reveal) {
        return false;
    }
    *lane = reveal->first;
    *x = reveal->second;
    return true;
}

void geo_gallery_atlas_camera(const GeoGallery* gallery, float* out) {
    const gallery::AtlasCamera cam = gallery->gallery.atlasCamera();
    out[0] = cam.k;
    out[1] = cam.x;
    out[2] = cam.y;
    out[3] = cam.fit;
}

void geo_gallery_set_atlas_camera(GeoGallery* gallery, float k, float x, float y) {
    gallery->gallery.setAtlasCamera(k, x, y);
}

void geo_gallery_atlas_zoom(GeoGallery* gallery, float factor, float x, float y) {
    const gallery::AtlasCamera cam = gallery->gallery.atlasCamera();
    const float k = std::clamp(cam.k * factor, cam.fit * 0.6f, cam.fit * 12.0f);
    gallery->gallery.setAtlasCamera(k, x - (x - cam.x) * k / cam.k, y - (y - cam.y) * k / cam.k);
}

void geo_gallery_atlas_pan(GeoGallery* gallery, float dx, float dy) {
    const gallery::AtlasCamera cam = gallery->gallery.atlasCamera();
    gallery->gallery.setAtlasCamera(cam.k, cam.x + dx, cam.y + dy);
}

void geo_gallery_atlas_level(GeoGallery* gallery, uint32_t level) {
    const gallery::AtlasCamera cam = gallery->gallery.atlasCamera();
    if (level == 0) {
        gallery->gallery.resetAtlasCamera();
        return;
    }
    const float stops[3] = {1.0f, 2.2f, 5.0f};
    const float k = cam.fit * stops[std::min<uint32_t>(level, 2)];
    const float cx = gallery->gallery.viewWidth() / 2.0f, cy = gallery->gallery.viewHeight() / 2.0f;
    gallery->gallery.setAtlasCamera(k, cx - (cx - cam.x) * k / cam.k,
                                    cy - (cy - cam.y) * k / cam.k);
}

void geo_gallery_catalogue(const GeoGallery* gallery, uint32_t* out) {
    out[0] = gallery->gallery.catalogueSpread();
    out[1] = gallery->gallery.catalogueSpreads();
}

void geo_gallery_set_catalogue_spread(GeoGallery* gallery, uint32_t spread) {
    gallery->gallery.setCatalogueSpread(spread);
}

void geo_gallery_catalogue_index(GeoGallery* gallery) {
    gallery->gallery.setCatalogueSpread(gallery->gallery.catalogueIndexSpread());
}

void geo_gallery_set_ladders(GeoGallery* gallery, bool shared) {
    gallery->gallery.setLadders(shared);
}

void geo_gallery_set_lost(GeoGallery* gallery, bool lost) {
    gallery->gallery.setLost(lost);
}

void geo_gallery_set_reading(GeoGallery* gallery, uint32_t file, float fraction) {
    if (file < gallery->gallery.tree().fileCount()) {
        gallery->gallery.setReading(file, fraction);
    }
}

void geo_gallery_set_changed(GeoGallery* gallery, uint32_t file, bool changed) {
    if (file < gallery->gallery.tree().fileCount()) {
        gallery->gallery.setChanged(file, changed);
    }
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
