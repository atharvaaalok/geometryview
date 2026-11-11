// The gallery's layout core (gallery.h) for the page, in the engine's module: the same display
// list the Mac paints, read by Web/viewer/gallery.js from the module's memory. Strings and arrays
// cross as pointers into the heap; everything else as numbers.
#include "engine.h"
#include "gallery.h"

#include <emscripten/emscripten.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {

EMSCRIPTEN_KEEPALIVE GeoGallery* gv_gallery_create() {
    return geo_gallery_create();
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_destroy(GeoGallery* gallery) {
    geo_gallery_destroy(gallery);
}

/// Entries as a buffer of paths each ended by a newline, with a byte per entry for "is a folder"
/// and a double each for bytes and modification time.
EMSCRIPTEN_KEEPALIVE void gv_gallery_set_entries(GeoGallery* gallery, const char* title,
                                                 const char* paths, const uint8_t* folders,
                                                 const double* bytes, const double* modified,
                                                 uint32_t count) {
    std::vector<std::string> owned;
    owned.reserve(count);
    const char* at = paths;
    for (uint32_t i = 0; i < count; ++i) {
        const char* end = std::strchr(at, '\n');
        if (end == nullptr) {
            end = at + std::strlen(at);
        }
        owned.emplace_back(at, end);
        at = *end == '\n' ? end + 1 : end;
    }
    std::vector<GeoGalleryEntry> entries(owned.size());
    for (size_t i = 0; i < owned.size(); ++i) {
        entries[i] = {owned[i].c_str(), folders[i] != 0, uint64_t(bytes[i]), modified[i]};
    }
    geo_gallery_set_entries(gallery, title, entries.data(), uint32_t(entries.size()));
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_file_count(GeoGallery* gallery) {
    return geo_gallery_file_count(gallery);
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_file_path(GeoGallery* gallery, uint32_t file) {
    return geo_gallery_file_path(gallery, file);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_file_at(GeoGallery* gallery, const char* path) {
    return geo_gallery_file_at(gallery, path);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_facts(GeoGallery* gallery, uint32_t file, double x,
                                               double y, double z, int hasBox, int estimated,
                                               uint32_t parts, const char* product,
                                               double triangles) {
    const GeoGalleryFacts facts{{x, y, z}, hasBox != 0, estimated != 0,
                                parts,     product,     uint64_t(triangles)};
    geo_gallery_set_facts(gallery, file, &facts);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_picture(GeoGallery* gallery, uint32_t file, uint32_t width,
                                                 uint32_t height, double mm, const float* part,
                                                 const float* ink) {
    GeoGalleryPicture picture{width, height, mm, {}, {}};
    std::memcpy(picture.part, part, sizeof(picture.part));
    std::memcpy(picture.ink, ink, sizeof(picture.ink));
    geo_gallery_set_picture(gallery, file, &picture);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_failure(GeoGallery* gallery, uint32_t file,
                                                 const char* message) {
    geo_gallery_set_failure(gallery, file, message);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_reading(GeoGallery* gallery, uint32_t file,
                                                 float fraction) {
    geo_gallery_set_reading(gallery, file, fraction);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_changed(GeoGallery* gallery, uint32_t file, int changed) {
    geo_gallery_set_changed(gallery, file, changed != 0);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_style(GeoGallery* gallery, uint32_t style) {
    geo_gallery_set_style(gallery, style);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_viewport(GeoGallery* gallery, float width, float height) {
    geo_gallery_set_viewport(gallery, width, height);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_scroll(GeoGallery* gallery, float top) {
    geo_gallery_set_scroll(gallery, top);
}

EMSCRIPTEN_KEEPALIVE int gv_gallery_set_folder(GeoGallery* gallery, const char* path) {
    return geo_gallery_set_folder(gallery, path) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_folder(GeoGallery* gallery) {
    return geo_gallery_folder(gallery);
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_folder_title(GeoGallery* gallery) {
    return geo_gallery_folder_title(gallery);
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_folder_subtitle(GeoGallery* gallery) {
    return geo_gallery_folder_subtitle(gallery);
}

EMSCRIPTEN_KEEPALIVE int gv_gallery_layout(GeoGallery* gallery) {
    return geo_gallery_layout(gallery) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_item_count(GeoGallery* gallery) {
    return geo_gallery_item_count(gallery);
}

EMSCRIPTEN_KEEPALIVE const GeoGalleryItem* gv_gallery_items(GeoGallery* gallery) {
    return geo_gallery_items(gallery);
}

/// The size of one item in the heap, so the page reads them with the right stride.
EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_item_size() {
    return sizeof(GeoGalleryItem);
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_text(GeoGallery* gallery) {
    return geo_gallery_text(gallery);
}

EMSCRIPTEN_KEEPALIVE float gv_gallery_content_height(GeoGallery* gallery) {
    return geo_gallery_content_height(gallery);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_picture_pixels(GeoGallery* gallery) {
    return geo_gallery_picture_pixels(gallery);
}

/// Hits a point: kind, target and key written to `out`.
EMSCRIPTEN_KEEPALIVE void gv_gallery_hit(GeoGallery* gallery, float x, float y, uint32_t* out) {
    const GeoGalleryHit hit = geo_gallery_hit(gallery, x, y);
    out[0] = hit.target_kind;
    out[1] = hit.target;
    out[2] = hit.key;
}

EMSCRIPTEN_KEEPALIVE int gv_gallery_hover(GeoGallery* gallery, uint32_t kind, uint32_t target) {
    return geo_gallery_hover(gallery, kind, target) ? 1 : 0;
}

/// A click or key: the action's kind and target written to `out`.
EMSCRIPTEN_KEEPALIVE void gv_gallery_click(GeoGallery* gallery, uint32_t kind, uint32_t target,
                                           int twice, uint32_t* out) {
    const GeoGalleryAction action = geo_gallery_click(gallery, kind, target, twice != 0);
    out[0] = action.kind;
    out[1] = action.target;
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_key(GeoGallery* gallery, uint32_t key, uint32_t* out) {
    const GeoGalleryAction action = geo_gallery_key(gallery, key);
    out[0] = action.kind;
    out[1] = action.target;
}

/// What the gallery wants drawn: file, band, order and folder colour, four numbers a file, as
/// doubles in `out`, at most `capacity` files. Returns how many there are.
EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_wanted(GeoGallery* gallery, double* out,
                                                uint32_t capacity) {
    const uint32_t n = geo_gallery_wanted(gallery, nullptr, 0);
    std::vector<GeoGalleryWant> wants(n);
    geo_gallery_wanted(gallery, wants.data(), n);
    for (uint32_t i = 0; i < n && i < capacity; ++i) {
        out[i * 4 + 0] = wants[i].file;
        out[i * 4 + 1] = wants[i].band;
        out[i * 4 + 2] = wants[i].order;
        out[i * 4 + 3] = wants[i].folder_color;
    }
    return n;
}

/// The alignment check of what the page painted: lines and columns per item, NaN where not painted.
/// Writes group, column, spread and members as doubles, four a group over `tolerance`, at most
/// `capacity`; returns how many are over, and the groups measured in out_checked[0].
EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_check(GeoGallery* gallery, const float* lines,
                                               const float* columns, float tolerance, double* out,
                                               uint32_t capacity, uint32_t* checked) {
    std::vector<GeoGalleryGroupSpread> spreads(capacity);
    const uint32_t over =
        geo_gallery_check(gallery, lines, columns, tolerance, spreads.data(), capacity, checked);
    for (uint32_t i = 0; i < over && i < capacity; ++i) {
        out[i * 4 + 0] = spreads[i].group;
        out[i * 4 + 1] = spreads[i].column ? 1 : 0;
        out[i * 4 + 2] = spreads[i].spread;
        out[i * 4 + 3] = spreads[i].items;
    }
    return over;
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_views(GeoGallery* gallery, uint32_t mask) {
    geo_gallery_set_views(gallery, mask);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_views(GeoGallery* gallery) {
    return geo_gallery_views(gallery);
}

EMSCRIPTEN_KEEPALIVE int gv_gallery_set_view(GeoGallery* gallery, uint32_t view) {
    return geo_gallery_set_view(gallery, view) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_view(GeoGallery* gallery) {
    return geo_gallery_view(gallery);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_sheet(GeoGallery* gallery, uint32_t scale, float cell,
                                               uint32_t sort) {
    geo_gallery_set_sheet(gallery, scale, cell, sort);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_set_search(GeoGallery* gallery, const char* query) {
    geo_gallery_set_search(gallery, query);
}

EMSCRIPTEN_KEEPALIVE const char* gv_gallery_search(GeoGallery* gallery) {
    return geo_gallery_search(gallery);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_matches(GeoGallery* gallery) {
    return geo_gallery_matches(gallery);
}

EMSCRIPTEN_KEEPALIVE void gv_gallery_peek(GeoGallery* gallery, uint32_t file) {
    geo_gallery_peek(gallery, file);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_peeking(GeoGallery* gallery) {
    return geo_gallery_peeking(gallery);
}

EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_selected(GeoGallery* gallery) {
    return geo_gallery_selected(gallery);
}

EMSCRIPTEN_KEEPALIVE float gv_gallery_reveal(GeoGallery* gallery) {
    return geo_gallery_reveal(gallery);
}

/// The last layout's transition as 7 floats: kind, the rect (x, y, width, height), duration and the
/// old view's fade.
EMSCRIPTEN_KEEPALIVE void gv_gallery_transition(GeoGallery* gallery, float* out) {
    const GeoGalleryTransition t = geo_gallery_transition(gallery);
    const float values[7] = {float(t.kind), t.rect[0],  t.rect[1], t.rect[2],
                             t.rect[3],     t.duration, t.fade_out};
    std::memcpy(out, values, sizeof(values));
}

/// The last layout's tracks, 8 floats each: item, kind, from (x, y, width, height), delay and
/// duration, at most `capacity`. Returns how many there are.
EMSCRIPTEN_KEEPALIVE uint32_t gv_gallery_tracks(GeoGallery* gallery, float* out,
                                                uint32_t capacity) {
    const uint32_t n = geo_gallery_tracks(gallery, nullptr, 0);
    std::vector<GeoGalleryTrack> tracks(n);
    geo_gallery_tracks(gallery, tracks.data(), n);
    for (uint32_t i = 0; i < n && i < capacity; ++i) {
        const GeoGalleryTrack& t = tracks[i];
        const float values[8] = {float(t.item), float(t.kind), t.from[0], t.from[1],
                                 t.from[2],     t.from[3],     t.delay,   t.duration};
        std::memcpy(out + i * 8, values, sizeof(values));
    }
    return n;
}

} // extern "C"

extern "C" {

/// A style's sheet (engine.h): top and bottom colours, vignette, grid colour, alpha and pitch, and
/// whether it is dark, as 13 floats.
EMSCRIPTEN_KEEPALIVE void gv_style_sheet(uint32_t style, float* out) {
    const GeoSheet sheet = geo_style_sheet(GeoStyle(style));
    const float values[13] = {sheet.top[0],
                              sheet.top[1],
                              sheet.top[2],
                              sheet.bottom[0],
                              sheet.bottom[1],
                              sheet.bottom[2],
                              sheet.vignette,
                              sheet.grid[0],
                              sheet.grid[1],
                              sheet.grid[2],
                              sheet.grid_alpha,
                              sheet.grid_pitch,
                              sheet.on_dark ? 1.0f : 0.0f};
    std::memcpy(out, values, sizeof(values));
}

} // extern "C"
