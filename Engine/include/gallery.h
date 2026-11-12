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
    GeoItemFixed = 16,  // placed in the view, not the content: it does not scroll (Peek)
    GeoItemSheet = 32,  // a rectangle filled with the drawing style's own sheet, as the gallery's
    GeoItemStale = 64,  // drawn at 45%: a picture being drawn again, its file changed
    GeoItemLost = 128,  // drawn at 35%: a folder that can no longer be read, as last seen
    GeoItemRuler = 256, // a rectangle drawn as a ruler: its top line, minor ticks every `radius`
                        // points, major ticks every `tracking` (Ladders)
} GeoGalleryItemFlags;

/// What a click on an item means.
typedef enum GeoGalleryTargetKind {
    GeoTargetNone = 0,
    GeoTargetFolder = 1,
    GeoTargetFile = 2,
    GeoTargetMore = 3,      // "show all" of a size band cut short
    GeoTargetPeek = 4,      // Peek's card (0), where a click does nothing, or its close button (1)
    GeoTargetReconnect = 5, // the way back to a folder that can no longer be read
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
    // A heading that stays at the top of the view while its block scrolls under it: it moves
    // down by how far the view has scrolled past `sticky_top`, by `sticky_until` at most. Both 0
    // for an item that scrolls with the content.
    float sticky_top;
    float sticky_until;
    // A lane that scrolls sideways on its own (a strip of Ladders), or 0: the item is placed in the
    // lane's content, which the painter clips to the lane and moves by its scroll.
    uint32_t lane;
} GeoGalleryItem;

/// A lane: a band of the content that scrolls sideways on its own, clipped to its rect (in content
/// points), its content `content_width` wide.
typedef struct GeoGalleryLane {
    uint32_t id;
    float rect[4];
    float content_width;
} GeoGalleryLane;

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
    uint64_t triangles;  // as drawn, 0 when not known
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
    GeoActionReconnect = 6,   // ask again to read the folder
} GeoGalleryActionKind;

typedef struct GeoGalleryAction {
    uint32_t kind;
    uint32_t target;
} GeoGalleryAction;

/// The five views, in the order of the switch.
typedef enum GeoGalleryViewKind {
    GeoViewPlates = 0,
    GeoViewSheet = 1,
    GeoViewLadders = 2,
    GeoViewAtlas = 3,
    GeoViewCatalogue = 4,
} GeoGalleryViewKind;

/// How Sheet scales parts.
typedef enum GeoGalleryScale {
    GeoScaleFit = 0,    // each part fills its cell
    GeoScaleFolder = 1, // one scale a folder
    GeoScaleTrue = 2,   // one scale for everything in view
} GeoGalleryScale;

typedef enum GeoGallerySort {
    GeoSortName = 0,
    GeoSortSize = 1,
} GeoGallerySort;

/// What a layout should play, against the one before it (Docs/design/gallery-spec.md, "Motion"),
/// with the ease (0.3, 0.7, 0.2, 1) throughout, and nothing under Reduce Motion.
typedef enum GeoGalleryTransitionKind {
    GeoTransitionNone = 0,
    GeoTransitionSwitch = 1, // views switched: parts in both fly, the old view fades out
    GeoTransitionInto = 2,   // into a folder: it grows out of its plate
    GeoTransitionOut = 3,    // up a folder: it shrinks back into the plate it came from
} GeoGalleryTransitionKind;

typedef struct GeoGalleryTransition {
    uint32_t kind;
    float rect[4];  // the plate it grows from or shrinks into, in the view: x, y, width, height
    float duration; // milliseconds
    float fade_out; // the old view's fade, milliseconds
} GeoGalleryTransition;

typedef enum GeoGalleryTrackKind {
    GeoTrackFly = 0,    // from where it was in the old view to its place
    GeoTrackFadeIn = 1, // new to the view: fades in
} GeoGalleryTrackKind;

typedef struct GeoGalleryTrack {
    uint32_t item; // in the new display list
    uint32_t kind;
    float from[4];         // where it starts, in the view
    float delay, duration; // milliseconds
} GeoGalleryTrack;

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

/// The views that may be shown, a bit each (1 << GeoViewPlates ...): Plates and Sheet unless told.
void geo_gallery_set_views(GeoGallery* gallery, uint32_t mask);
uint32_t geo_gallery_views(const GeoGallery* gallery);
/// Shows a view, if enabled; the folder in view remembers it. Selection, search, hover and style
/// carry across.
bool geo_gallery_set_view(GeoGallery* gallery, uint32_t view);
uint32_t geo_gallery_view(const GeoGallery* gallery);

/// Sheet's controls: its scale (GeoGalleryScale), its cells' size in points (72 to 200) and its
/// order (GeoGallerySort).
void geo_gallery_set_sheet(GeoGallery* gallery, uint32_t scale, float cell, uint32_t sort);

/// Atlas's camera: view points = map units × k + (x, y), as 4 floats: k, x, y, and the k that fits
/// the whole map.
void geo_gallery_atlas_camera(const GeoGallery* gallery, float* out);
void geo_gallery_set_atlas_camera(GeoGallery* gallery, float k, float x, float y);
/// Zooms the map by `factor` about a point of the view, or pans it by (dx, dy) view points.
void geo_gallery_atlas_zoom(GeoGallery* gallery, float factor, float x, float y);
void geo_gallery_atlas_pan(GeoGallery* gallery, float dx, float dy);
/// Zooms to a level, about the view's middle: 0 the whole map (regions), 1 districts, 2 every part.
void geo_gallery_atlas_level(GeoGallery* gallery, uint32_t level);

/// Ladders' scale: each strip its own (false), or one for every strip (true).
void geo_gallery_set_ladders(GeoGallery* gallery, bool shared);

/// Searches the folder in view: names, the names inside files, and folders. Empty clears it.
void geo_gallery_set_search(GeoGallery* gallery, const char* query);
const char* geo_gallery_search(const GeoGallery* gallery);
/// Files in the folder that the search found, or UINT32_MAX with no search.
uint32_t geo_gallery_matches(const GeoGallery* gallery);

/// Shows the view as a poster, to export: printed on one sheet, the folder's title above and a
/// footer rule below with `date`, every cell drawn and nothing hovered, selected or peeked. The
/// front end sets the viewport to the page it prints.
void geo_gallery_set_poster(GeoGallery* gallery, bool poster, const char* date);
bool geo_gallery_poster(const GeoGallery* gallery);

/// The lanes of the last layout, at most `capacity`; returns how many there are.
uint32_t geo_gallery_lanes(const GeoGallery* gallery, GeoGalleryLane* out, uint32_t capacity);
/// How far a lane is scrolled sideways, which hits take into account; kept across layouts.
void geo_gallery_set_lane_scroll(GeoGallery* gallery, uint32_t lane, float x);

/// Where to scroll the selection's lane so it shows, after a key: true with the lane and its
/// scroll, false if it is in no lane or shows already.
bool geo_gallery_reveal_lane(const GeoGallery* gallery, uint32_t* lane, float* x);

/// The folder can no longer be read (in the browser, its permission lapsed): what was last seen
/// shows faded, under a card with one way back, Reconnect, whose click returns GeoActionReconnect.
void geo_gallery_set_lost(GeoGallery* gallery, bool lost);

/// A file being read: how far, from 0 to 1, shown as a mustard bar on its measured box; negative
/// once it is not. A picture ends it.
void geo_gallery_set_reading(GeoGallery* gallery, uint32_t file, float fraction);
/// A file changed on disk: a Δ marks it until it is seen (hovered, selected, peeked or opened), and
/// its picture shows faded until a new one arrives. Nothing else moves.
void geo_gallery_set_changed(GeoGallery* gallery, uint32_t file, bool changed);

/// Shows a file large, with its measured size, parts, triangles and file, over the view; UINT32_MAX
/// closes it.
void geo_gallery_peek(GeoGallery* gallery, uint32_t file);
uint32_t geo_gallery_peeking(const GeoGallery* gallery);

/// Where to scroll so the selection shows, after a switch or a key; negative if it shows already.
float geo_gallery_reveal(const GeoGallery* gallery);

/// What the last layout should play against the one before, and its tracks: read right after a
/// layout that returned true.
GeoGalleryTransition geo_gallery_transition(const GeoGallery* gallery);
uint32_t geo_gallery_tracks(const GeoGallery* gallery, GeoGalleryTrack* out, uint32_t capacity);

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
