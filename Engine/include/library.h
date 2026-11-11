// The gallery's library: facts and pictures for whole folders of STEP and STL files, from the
// caches where it can and by importing and drawing where it must. One per gallery window.
//
// Calls never block, except geo_library_scan and geo_library_wait_idle, which say so. Events
// arrive on a thread of the library's own, in the order learned.
#ifndef GEOMETRYVIEW_LIBRARY_H
#define GEOMETRYVIEW_LIBRARY_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeoLibrary GeoLibrary;

/// How much a file is wanted, most first.
typedef enum GeoLibraryBand {
    GeoBandOpen = 0,     // being opened
    GeoBandVisible = 1,  // in view
    GeoBandNear = 2,     // near the view
    GeoBandPrefetch = 3, // anywhere else in the gallery, while nothing else waits
} GeoLibraryBand;

typedef struct GeoLibraryWant {
    const char* path;
    GeoLibraryBand band;
    double order; // within its band, least first
    uint64_t bytes;
    double modified; // seconds since 1970, from a scan: a change looks the file up again
    // The colour a one-part file's part takes (its folder's, 0 mustard, 1 mint, 2 tomato,
    // 3 blush), or -1 for the cycle's own.
    int32_t folder_color;
} GeoLibraryWant;

/// What is known of a file without drawing it. Strings last only as long as the event.
typedef struct GeoFacts {
    double min[3], max[3]; // millimetres
    bool has_box;
    bool estimated; // from the file's structure, before its geometry was read
    uint64_t triangles, faces;
    uint32_t parts, bodies, definitions;
    const char* name; // the name inside the file, or empty
} GeoFacts;

/// A picture, kept as a PNG. Its alpha is premultiplied once read back.
typedef struct GeoPicture {
    const char* png_path;
    uint32_t width, height;
    double world_per_pixel; // millimetres a pixel
    uint32_t part_rect[4];  // the part, without its shadow: x, y, width, height
    uint32_t ink_rect[4];   // everything drawn
    int32_t first_color;
} GeoPicture;

typedef enum GeoLibraryEventKind {
    GeoLibraryFacts = 0,
    GeoLibraryPicture = 1,
    GeoLibraryProgress = 2,
    GeoLibraryFailed = 3,
    GeoLibraryTurnFrame = 4, // a turntable's frame: `done` of `total`, in `picture`
} GeoLibraryEventKind;

typedef struct GeoLibraryEvent {
    GeoLibraryEventKind kind;
    const char* path;
    GeoFacts facts;       // GeoLibraryFacts
    GeoPicture picture;   // GeoLibraryPicture
    uint32_t done, total; // GeoLibraryProgress, in slices; GeoLibraryTurnFrame, frame of frames
    const char* message;  // GeoLibraryFailed
} GeoLibraryEvent;

typedef void (*GeoLibraryCallback)(void* context, const GeoLibraryEvent* event);

typedef struct GeoLibraryDescriptor {
    const char* shader_library_path;
    const char* import_helper_path;
    // The import cache, as the engine's; the gallery keeps its facts, pictures and listings in
    // `gallery/` under it.
    const char* cache_directory;
    GeoLibraryCallback callback;
    void* context;
    uint32_t slots;   // imports at once, or 0 for max(2, performance cores / 2)
    uint32_t workers; // OCCT workers each, or 0 for 2
} GeoLibraryDescriptor;

/// How pictures are drawn: from the front right and above, on a clear sheet.
typedef struct GeoPictureSpec {
    GeoStyle style;
    GeoColorMode color_mode;
    uint32_t pixels; // the picture's side
    float pixels_per_point;
    float line_scale; // clamp(cell / 320, 0.5, 1)
} GeoPictureSpec;

/// Returns null if the caches cannot be made.
GeoLibrary* geo_library_create(const GeoLibraryDescriptor* descriptor);
/// Cancels what runs and waits for it.
void geo_library_destroy(GeoLibrary* library);

/// Replaces the files wanted. Paths are copied.
void geo_library_want(GeoLibrary* library, const GeoLibraryWant* wants, uint32_t count);
void geo_library_set_picture(GeoLibrary* library, const GeoPictureSpec* spec);
void geo_library_set_prefetch(GeoLibrary* library, bool allowed);
/// Peek's turntable: `frames` pictures of the file at `path`, `pixels` square, turned evenly about
/// the upright at one scale, ahead of any other picture, each told as a GeoLibraryTurnFrame.
/// 0 frames, or a null path, stops it.
void geo_library_turn(GeoLibrary* library, const char* path, uint32_t frames, uint32_t pixels);
/// Blocks until nothing waits or runs: for tools and tests.
void geo_library_wait_idle(GeoLibrary* library);

typedef struct GeoLibraryStats {
    uint32_t helpers, imports, structures, pictures, lookups, failures, cancelled;
} GeoLibraryStats;
GeoLibraryStats geo_library_stats(GeoLibrary* library);

/// A file or folder found by a scan, its path relative to the root.
typedef struct GeoScanEntry {
    const char* path;
    uint64_t bytes;
    double modified; // seconds since 1970
    bool folder;
    uint32_t files; // a folder's STEP and STL files, at any depth
} GeoScanEntry;

/// Receives entries in batches; returns false to stop.
typedef bool (*GeoScanCallback)(void* context, const GeoScanEntry* entries, uint32_t count);

typedef struct GeoScanSummary {
    uint32_t files, folders;
    bool truncated; // at 20,000 files or a depth of 32
    bool stopped;
} GeoScanSummary;

/// Lists a folder's STEP and STL files and its folders, in batches as they are found, and keeps
/// the listing for next time. Blocks: call it off the main thread. Returns false with the reason
/// in `error` if the folder cannot be read.
bool geo_library_scan(GeoLibrary* library, const char* root, GeoScanCallback callback,
                      void* context, GeoScanSummary* summary, char* error, uint32_t error_capacity);

/// The listing kept from a folder's last scan, in one batch, so a gallery shows it at once.
/// Returns false if there is none.
bool geo_library_kept_listing(GeoLibrary* library, const char* root, GeoScanCallback callback,
                              void* context);

#ifdef __cplusplus
}
#endif

#endif
