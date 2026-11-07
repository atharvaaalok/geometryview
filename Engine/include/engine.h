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
    GeoPointerLeave = 5,  // the pointer left the view: nothing is hovered until it returns
    GeoPointerCancel = 6, // a press ended where the view could not see it: no click results
} GeoPointerKind;

typedef enum GeoPointerButton {
    GeoButtonNone = 0,
    GeoButtonLeft = 1,
    GeoButtonRight = 2,
    GeoButtonMiddle = 3,
} GeoPointerButton;

/// Modifier bits in pointer events and mouse bindings.
enum {
    GeoModifierShift = 1 << 0,
    GeoModifierControl = 1 << 1,
    GeoModifierOption = 1 << 2,
    GeoModifierCommand = 1 << 3,
    GeoModifierSpace = 1 << 4, // the space bar held
};

/// What a drag with a mouse button does.
typedef enum GeoGesture {
    GeoGestureNone = 0,
    GeoGestureRotate = 1,
    GeoGesturePan = 2,
    GeoGestureDolly = 3,
} GeoGesture;

/// One row of the mouse mapping. The most specific row that matches a press wins.
typedef struct GeoMouseBinding {
    uint32_t button; // a GeoPointerButton
    uint32_t modifiers;
    GeoGesture gesture;
} GeoMouseBinding;

#define GEO_MAX_MOUSE_BINDINGS 16

typedef struct GeoMouseSettings {
    GeoMouseBinding bindings[GEO_MAX_MOUSE_BINDINGS];
    uint32_t binding_count;
    bool zoom_toward_pointer; // otherwise toward the centre of the view
    bool invert_wheel;
    float zoom_speed; // 1 is the default
} GeoMouseSettings;

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

/// Render styles, after step_viewer's.
typedef enum GeoStyle {
    GeoStyleIllustration = 0, // flat light, ink and paper
    GeoStyleMetal = 1,        // studio light on machined parts
    GeoStyleClay = 2,         // one matte material, soft light
    GeoStyleBlueprint = 3,    // white lines on a blue sheet
    GeoStyleTechnical = 4,    // black linework on white
} GeoStyle;

/// What decides each body's colour.
typedef enum GeoColorMode {
    GeoColorMaterial = 0, // by what each part is made of, read from its name
    GeoColorParts = 1,    // one colour per part
    GeoColorMono = 2,     // one material throughout
    GeoColorFile = 3,     // colours stored in the file
} GeoColorMode;

/// How ghosts are drawn.
typedef enum GeoGhostStyle {
    GeoGhostShell = 1, // a faint skin, outline kept
    GeoGhostLines = 2, // dashed edges only
    GeoGhostBoth = 3,  // skin and dashed edges
} GeoGhostStyle;

/// Everything that decides how the model is drawn.
typedef struct GeoLook {
    GeoStyle style;
    GeoColorMode color_mode;
    GeoGhostStyle ghost_style;
    bool cast_shadow;
    float grain;  // paper grain, 0 to 1, in styles that have it
    float wobble; // hand-drawn wobble in points, 0 to 7, in styles that have it
} GeoLook;

/// The look a new engine starts with: Illustration, by material, shell ghosts, a shadow.
GeoLook geo_default_look(void);

/// Whether a style casts a shadow unless asked not to. Blueprint and Technical do not.
bool geo_style_casts_shadow(GeoStyle style);

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
    GeoActionToggleSeeThrough = 18, // fade the outer shell to see the mechanism inside
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
    GeoEventHoverChanged = 6, // node: the part under the pointer, or GEO_NO_NODE; done: the face
    GeoEventLooksChanged = 7, // swatch colours changed: read them again with geo_engine_swatch
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

/// What is selected, for the selection card.
typedef struct GeoSelectionSummary {
    uint32_t occurrences; // bodies with anything selected
    uint32_t faces;       // faces selected, counting every face of a whole body
    uint32_t document;    // the tree row the selection belongs to, if node is not GEO_NO_NODE
    uint32_t node;
    uint32_t mode; // a GeoSelectionMode
} GeoSelectionSummary;

GeoSelectionSummary geo_engine_selection(GeoEngine* engine);

/// Counters for the debug overlay and for tests.
typedef struct GeoEngineStats {
    uint64_t frames_rendered;
    double last_frame_seconds;
    uint64_t last_frame_allocations; // counted only when GEOMETRY_ALLOCS is set
    uint32_t hovered_id;             // 0 when nothing is under the pointer
    uint32_t selected_count;         // occurrences with anything selected
    uint32_t hidden_count;           // occurrences with anything hidden
    double gpu_frame_seconds;        // mean GPU time per frame, measured only when tracing
    double gpu_frame_p99_seconds;    // the 99th percentile of the latest frames, likewise
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

/// Changes how the model is drawn.
void geo_engine_set_look(GeoEngine* engine, const GeoLook* look);

/// The mouse mapping a new engine starts with: left rotates, middle pans, right dollies, and Shift
/// or Space with left pans; the wheel zooms toward the pointer.
GeoMouseSettings geo_default_mouse_settings(void);

void geo_engine_set_mouse(GeoEngine* engine, const GeoMouseSettings* settings);

/// Section cuts along the model's axes. Each cut removes everything past a plane across one axis.
typedef struct GeoSection {
    bool enabled[3];   // cuts across x, y and z
    float position[3]; // where, from 0 to 1 across the model's bounds
    bool flip[3];      // remove the low side rather than the high side
} GeoSection;

void geo_engine_set_section(GeoEngine* engine, const GeoSection* section);

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

/// The colour standing for an occurrence under the current look, for swatches: RGBA8 with red in
/// the low byte, or 0 before its look is known. `occurrence` counts within the document, as a
/// node's first_occurrence does. Callable from any thread.
uint32_t geo_engine_swatch(GeoEngine* engine, uint32_t document, uint32_t occurrence);

/// Requests a redraw, for changes that do not arrive as input.
void geo_engine_request_frame(GeoEngine* engine);

GeoEngineStats geo_engine_stats(const GeoEngine* engine);

/// Tells the engine which edges of the view the interface covers, in points, so framing keeps the
/// model in the part left clear.
void geo_engine_set_view_insets(GeoEngine* engine, float left, float top, float right,
                                float bottom);

/// Writes the current view, as drawn, to a PNG at `path`, on the engine thread. The file appears
/// once it is complete.
void geo_engine_write_snapshot(GeoEngine* engine, const char* path);

/// Prints input-to-present latency to stderr when GEOMETRY_LATENCY is set.
void geo_engine_report_latency(GeoEngine* engine);

/// Prints current and peak memory footprint to stderr when GEOMETRY_TRACE is set.
void geo_trace_memory(const char* label);

/// How to render a cache file headlessly.
typedef struct GeoRenderOptions {
    uint32_t width, height; // pixels, drawn as for a 2x display
    double azimuth_degrees; // the view direction
    double elevation_degrees;
    bool perspective;
    bool overlay; // the view cube and scale bar
    GeoLook look;
    GeoSection section;
} GeoRenderOptions;

/// The options gcli render starts from: 1600 by 1000, from the front right and above, with the
/// overlay and the default look.
GeoRenderOptions geo_default_render_options(void);

/// Renders a cache file to a PNG, framed to fit, for tests and `gcli render`. Returns false and
/// prints why on failure.
bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, const GeoRenderOptions* options);

#ifdef __cplusplus
}
#endif

#endif // GEOMETRY_ENGINE_H
