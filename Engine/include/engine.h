// The Geometry engine's public interface. This is the only header the Swift app sees.
//
// Every function here is safe to call from the main thread and never blocks: calls enqueue work for
// the engine thread, which is the single writer of all view state.
#ifndef GEOMETRY_ENGINE_H
#define GEOMETRY_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeoEngine GeoEngine;

typedef enum GeoPointerKind {
    GeoPointerMove = 0,
    GeoPointerDown = 1,
    GeoPointerUp = 2,
    GeoPointerScroll = 3,
    GeoPointerMagnify = 4,
} GeoPointerKind;

typedef enum GeoPointerButton {
    GeoButtonNone = 0,
    GeoButtonLeft = 1,
    GeoButtonRight = 2,
    GeoButtonMiddle = 3,
} GeoPointerButton;

/// Pointer input in drawable pixels, with the origin at the top left of the viewport.
typedef struct GeoPointerEvent {
    GeoPointerKind kind;
    GeoPointerButton button;
    float x;
    float y;
    float delta; // zoom amount for scroll and magnify; positive zooms in
    uint32_t modifiers;
    double timestamp;
} GeoPointerEvent;

/// Actions the app can invoke. Keys reach these through a table the settings UI can edit.
typedef enum GeoAction {
    GeoActionFit = 0,
    GeoActionClearSelection = 1,
    GeoActionToggleProjection = 2,
    GeoActionShowAll = 3,
    GeoActionUnghostAll = 4,
    GeoActionIsolate = 5,
    GeoActionHideSelection = 6,
    GeoActionGhostSelection = 7,
    GeoActionUndo = 8,
    GeoActionRedo = 9,
    GeoActionViewFront = 10,
    GeoActionViewBack = 11,
    GeoActionViewLeft = 12,
    GeoActionViewRight = 13,
    GeoActionViewTop = 14,
    GeoActionViewBottom = 15,
    GeoActionViewIso = 16,
    GeoActionFitSelection = 17,
} GeoAction;

/// What a click picks: one face, a whole body, or everything in its part.
typedef enum GeoSelectionMode {
    GeoSelectFaces = 0,
    GeoSelectBodies = 1,
    GeoSelectParts = 2,
} GeoSelectionMode;

/// What a click does, while a key is held: select, hide, or ghost (clicking a ghost restores it).
typedef enum GeoTool {
    GeoToolSelect = 0,
    GeoToolHide = 1,
    GeoToolGhost = 2,
} GeoTool;

typedef enum GeoEventKind {
    GeoEventLoading = 0,          // message: the file's name
    GeoEventSceneReady = 1,       // every document has finished importing
    GeoEventFailed = 2,           // message: why, in a sentence a person can read
    GeoEventProgress = 3,         // done and total, in units only the ratio of which matters
    GeoEventModelReady = 4,       // a document's tree can be read with geo_engine_copy_model
    GeoEventSelectionChanged = 5, // node: the tree row to reveal, or GEO_NO_NODE
} GeoEventKind;

#define GEO_NO_NODE UINT32_MAX

typedef struct GeoEvent {
    GeoEventKind kind;
    uint32_t document;
    uint32_t node;
    uint32_t done;
    uint32_t total;
    const char* message; // valid only during the callback
} GeoEvent;

/// Called on the engine thread.
typedef void (*GeoEventCallback)(void* context, const GeoEvent* event);

typedef struct GeoEngineDescriptor {
    const char* shader_library_path;
    const char* import_helper_path;
    const char* cache_directory;
    GeoEventCallback event_callback;
    void* event_context;
} GeoEngineDescriptor;

/// Counters for the debug overlay and for tests.
typedef struct GeoEngineStats {
    uint64_t frames_rendered;
    double last_frame_seconds;
    uint64_t last_frame_allocations; // counted only when GEOMETRY_ALLOCS is set
    uint32_t hovered_id;             // 0 when nothing is under the pointer
    uint32_t selected_count;         // occurrences with anything selected
    uint32_t hidden_count;           // occurrences with anything hidden
} GeoEngineStats;

/// Returns the engine version as a static string.
const char* geo_engine_version(void);

/// Creates the engine and starts its thread. Returns null if the GPU is unsupported or the shader
/// library cannot be loaded.
GeoEngine* geo_engine_create(const GeoEngineDescriptor* descriptor);
void geo_engine_destroy(GeoEngine* engine);

/// Hands the engine a CAMetalLayer. The caller configures its size; the engine retains it.
void geo_engine_attach_layer(GeoEngine* engine, void* metal_layer);

/// Notifies the engine that the viewport changed size, in drawable pixels.
void geo_engine_resize(GeoEngine* engine, uint32_t width, uint32_t height, float scale);

/// Posts pointer input. Never blocks; input is dropped only if the queue is full.
void geo_engine_post_pointer(GeoEngine* engine, const GeoPointerEvent* event);

/// Opens a file, replacing what is shown. Progress arrives through the event callback.
void geo_engine_open_file(GeoEngine* engine, const char* path);

/// Opens a file alongside what is shown, as another document.
void geo_engine_add_file(GeoEngine* engine, const char* path);

void geo_engine_perform(GeoEngine* engine, GeoAction action);

void geo_engine_set_selection_mode(GeoEngine* engine, GeoSelectionMode mode);

/// Sets the tool clicks use, while its key is held. Release it with GeoToolSelect.
void geo_engine_set_tool(GeoEngine* engine, GeoTool tool);

/// Selects everything under a tree node. With `extend` the node toggles instead.
void geo_engine_select_node(GeoEngine* engine, uint32_t document, uint32_t node, bool extend);

/// Lights up everything under a tree node, as hover does. GEO_NO_NODE clears it.
void geo_engine_hover_node(GeoEngine* engine, uint32_t document, uint32_t node);

// A document's assembly tree, readable from any thread. It holds the file's mapping, so it stays
// valid after the document closes, until released.

typedef struct GeoModel GeoModel;

typedef enum GeoNodeKind {
    GeoNodeAssembly = 0,
    GeoNodePart = 1,
    GeoNodeBody = 2,
} GeoNodeKind;

/// One tree row. Rows are depth-first, so a node's subtree is [index, subtree_end).
typedef struct GeoNode {
    uint32_t parent; // GEO_NO_NODE for a root
    uint32_t subtree_end;
    uint32_t first_occurrence;
    uint32_t occurrence_end;
    uint32_t name_offset;
    uint32_t name_length;
    uint32_t kind; // a GeoNodeKind
    uint32_t reserved;
} GeoNode;

/// Returns null if the document has no model yet.
GeoModel* geo_engine_copy_model(GeoEngine* engine, uint32_t document);
void geo_model_release(GeoModel* model);

uint32_t geo_model_node_count(const GeoModel* model);
const GeoNode* geo_model_nodes(const GeoModel* model);
/// Names are UTF-8, addressed by each node's name_offset and name_length.
const char* geo_model_names(const GeoModel* model);

/// Finds nodes whose names contain `query`, ignoring ASCII case, in tree order. Writes up to
/// `capacity` indices to `results` and returns how many matched in all.
uint32_t geo_model_search(GeoModel* model, const char* query, uint32_t* results, uint32_t capacity);

/// Requests a redraw, for changes that do not arrive as input.
void geo_engine_request_frame(GeoEngine* engine);

GeoEngineStats geo_engine_stats(const GeoEngine* engine);

/// Tells the engine which edges of the view the interface covers, in points, so framing keeps the
/// model in the part left clear.
void geo_engine_set_view_insets(GeoEngine* engine, float left, float top, float right,
                                float bottom);

/// Prints input-to-present latency to stderr when GEOMETRY_LATENCY is set.
void geo_engine_report_latency(GeoEngine* engine);

/// Prints current and peak memory footprint to stderr when GEOMETRY_TRACE is set.
void geo_trace_memory(const char* label);

/// Renders a cache file headlessly to a PNG, framed to fit, for tests and `gcli render`. The view
/// direction is given in degrees. Returns false and prints why on failure.
bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, uint32_t width, uint32_t height,
                            double azimuth_degrees, double elevation_degrees, bool perspective);

#ifdef __cplusplus
}
#endif

#endif // GEOMETRY_ENGINE_H
