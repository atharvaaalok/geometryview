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
} GeoAction;

typedef enum GeoEventKind {
    GeoEventLoading = 0,
    GeoEventSceneReady = 1,
    GeoEventFailed = 2,
} GeoEventKind;

/// Called on the engine thread. `message` is only valid during the call.
typedef void (*GeoEventCallback)(void* context, GeoEventKind kind, const char* message);

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
    uint32_t selected_id;            // 0 when nothing is selected
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

void geo_engine_perform(GeoEngine* engine, GeoAction action);

/// Requests a redraw, for changes that do not arrive as input.
void geo_engine_request_frame(GeoEngine* engine);

GeoEngineStats geo_engine_stats(const GeoEngine* engine);

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
