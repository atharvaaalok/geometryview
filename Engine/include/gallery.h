// The gallery's layout: a folder of files as a display list of rectangles, lines, text and
// pictures, which the Mac paints with Core Animation layers and the browser with the DOM. Every
// position and colour is decided here, once, so the two agree by construction.
//
// Coordinates are points (CSS pixels in the browser), y down, in the gallery's content: a painter
// shows the part between its scroll offset and the view's height.
#ifndef GEOMETRYVIEW_GALLERY_H
#define GEOMETRYVIEW_GALLERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeoGallery GeoGallery;

typedef enum GeoGalleryItemKind {
    GeoItemRect = 0,    // fill, then a stroke inside its edge, with rounded corners
    GeoItemText = 1,    // one line of IBM Plex Mono, set already: cut, in capitals
    GeoItemPicture = 2, // a file's picture, drawn whole into the item's rectangle
    GeoItemLine = 3,    // from (x, y) to (x + width, y + height)
} GeoGalleryItemKind;

typedef enum GeoGalleryItemFlags {
    GeoItemDashed = 1,  // a line or stroke in dashes: construction lines
    GeoItemHatched = 2, // a rectangle hatched at 45°: a file that could not be read
    GeoItemSticker = 4, // a rectangle lifted under the pointer: 3 pt shadow down and right
    GeoItemFading = 8,  // drawn at 22%: not what a search found
} GeoGalleryItemFlags;

/// What a click on an item means.
typedef enum GeoGalleryTargetKind {
    GeoTargetNone = 0,
    GeoTargetFolder = 1,
    GeoTargetFile = 2,
    GeoTargetMore = 3, // "show all" of a size band cut short
} GeoGalleryTargetKind;

typedef struct GeoGalleryItem {
    uint32_t kind;  // GeoGalleryItemKind
    uint32_t flags; // GeoGalleryItemFlags
    float x, y, width, height;
    uint32_t fill, stroke, shadow; // straight sRGB, red in the low byte; 0 for none
    float line_width, radius;
    uint32_t text, text_length; // the text's UTF-8 bytes, in the gallery's text buffer
    float font_size;
    float tracking; // points after every character
    uint32_t weight;
    uint32_t align; // 0 left, 1 centre, 2 right: within x .. x + width
    float baseline; // where the text stands
    uint32_t file;  // a picture's file
    uint32_t target_kind, target;
    uint32_t key; // the same thing keeps its key from one layout to the next
    // Alignment, checked by measurement: items of one group share a line, within half a point. A
    // row's line is a y (a baseline, a part's bottom, a rule); a column's an x (a left edge).
    uint32_t group; // 0 for none
    float line;
    uint32_t column; // 0 for none
    float column_line;
} GeoGalleryItem;

typedef struct GeoGalleryEntry {
    const char* path; // relative to the gallery's root
    bool folder;
    uint64_t bytes;
    double modified;
} GeoGalleryEntry;

typedef struct GeoGalleryFacts {
    double size[3]; // millimetres
    bool has_box;
    bool estimated;
    uint32_t parts;
    const char* product; // the name inside the file, or null
} GeoGalleryFacts;

typedef struct GeoGalleryPicture {
    uint32_t width, height; // pixels
    double mm_per_pixel;
    float part[4]; // the part alone: x, y, width, height in pixels
    float ink[4];  // everything drawn, its shadow included
} GeoGalleryPicture;

/// What a gallery wants drawn, in the library's bands, for one file.
typedef struct GeoGalleryWant {
    uint32_t file;
    uint32_t band; // 1 in view, 2 near, 3 elsewhere in the folder
    double order;
    int32_t folder_color;
} GeoGalleryWant;

typedef struct GeoGalleryHit {
    uint32_t target_kind, target;
    uint32_t key;
} GeoGalleryHit;

/// What a key or click asks of the front end.
typedef enum GeoGalleryActionKind {
    GeoActionNone = 0,
    GeoActionHandled = 1,     // the gallery changed itself: lay out and paint
    GeoActionEnterFolder = 2, // target: the folder now in view
    GeoActionOpenFile = 3,    // target: the file, to open in the viewer
    GeoActionPeek = 4,        // target: the file
    GeoActionLeave = 5,       // at the top already: close the gallery
} GeoGalleryActionKind;

typedef struct GeoGalleryAction {
    uint32_t kind;
    uint32_t target;
} GeoGalleryAction;

/// Keys the gallery reads.
typedef enum GeoGalleryKey {
    GeoKeyLeft = 0,
    GeoKeyRight = 1,
    GeoKeyUp = 2,
    GeoKeyDown = 3,
    GeoKeyEnter = 4,
    GeoKeySpace = 5,
    GeoKeyEscape = 6,
    GeoKeyFolderUp = 7, // ⌘↑
} GeoGalleryKey;

GeoGallery* geo_gallery_create(void);
void geo_gallery_destroy(GeoGallery* gallery);

/// Replaces the folder tree with a scan's entries; the root is named `title`. Facts, pictures and
/// the folder in view are cleared.
void geo_gallery_set_entries(GeoGallery* gallery, const char* title, const GeoGalleryEntry* entries,
                             uint32_t count);
uint32_t geo_gallery_file_count(const GeoGallery* gallery);
const char* geo_gallery_file_path(const GeoGallery* gallery, uint32_t file);
/// A file by its path, or UINT32_MAX.
uint32_t geo_gallery_file_at(const GeoGallery* gallery, const char* path);

void geo_gallery_set_facts(GeoGallery* gallery, uint32_t file, const GeoGalleryFacts* facts);
void geo_gallery_set_picture(GeoGallery* gallery, uint32_t file, const GeoGalleryPicture* picture);
void geo_gallery_set_failure(GeoGallery* gallery, uint32_t file, const char* message);

/// The drawing style, as engine.h numbers them: the sheet's colours follow it.
void geo_gallery_set_style(GeoGallery* gallery, uint32_t style);
/// The view's size, in points.
void geo_gallery_set_viewport(GeoGallery* gallery, float width, float height);
/// The part of the content in view, for which files are wanted first.
void geo_gallery_set_scroll(GeoGallery* gallery, float top);

/// Shows a folder by its path ("" for the root). Returns false if there is none.
bool geo_gallery_set_folder(GeoGallery* gallery, const char* path);
const char* geo_gallery_folder(const GeoGallery* gallery);
/// The folder in view's heading and its counts, for the masthead.
const char* geo_gallery_folder_title(const GeoGallery* gallery);
const char* geo_gallery_folder_subtitle(const GeoGallery* gallery);

/// Lays the folder out, if anything changed since the last time. Returns whether it did.
bool geo_gallery_layout(GeoGallery* gallery);
uint32_t geo_gallery_item_count(const GeoGallery* gallery);
const GeoGalleryItem* geo_gallery_items(const GeoGallery* gallery);
const char* geo_gallery_text(const GeoGallery* gallery);
float geo_gallery_content_height(const GeoGallery* gallery);
/// The side, in pixels, pictures are drawn at for this layout: twice the largest size shown.
uint32_t geo_gallery_picture_pixels(const GeoGallery* gallery);

/// What is under a point of the content.
GeoGalleryHit geo_gallery_hit(const GeoGallery* gallery, float x, float y);
/// The item under the pointer lifts; none for a target kind of GeoTargetNone.
bool geo_gallery_hover(GeoGallery* gallery, uint32_t target_kind, uint32_t target);
/// A click: a folder is entered, a file selected; twice, a file opens.
GeoGalleryAction geo_gallery_click(GeoGallery* gallery, uint32_t target_kind, uint32_t target,
                                   bool twice);
GeoGalleryAction geo_gallery_key(GeoGallery* gallery, uint32_t key);
/// The file selected, or UINT32_MAX.
uint32_t geo_gallery_selected(const GeoGallery* gallery);

/// The files to draw, in the library's bands: those in view, those near, the rest of the folder.
/// Writes at most `capacity`; returns how many there are.
uint32_t geo_gallery_wanted(const GeoGallery* gallery, GeoGalleryWant* out, uint32_t capacity);

/// One alignment group's spread, as painted.
typedef struct GeoGalleryGroupSpread {
    uint32_t group;
    bool column;  // a column of left edges, rather than a row's line
    float spread; // points between its lowest and highest member
    uint32_t items;
} GeoGalleryGroupSpread;

/// Checks the alignment of what a painter drew: `lines[i]` and `columns[i]` are where item i's
/// line and left edge were measured on screen, in content points, or NaN where it was not drawn.
/// Writes the groups whose spread passes `tolerance` (at most `capacity`), returns how many there
/// are, and sets `checked` to the groups measured.
uint32_t geo_gallery_check(const GeoGallery* gallery, const float* lines, const float* columns,
                           float tolerance, GeoGalleryGroupSpread* out, uint32_t capacity,
                           uint32_t* checked);

#ifdef __cplusplus
}
#endif

#endif
