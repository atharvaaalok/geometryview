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
                                               uint32_t parts, const char* product) {
    const GeoGalleryFacts facts{{x, y, z}, hasBox != 0, estimated != 0, parts, product};
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
