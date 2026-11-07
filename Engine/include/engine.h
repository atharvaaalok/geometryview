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
    float delta_x;
    float delta_y;
    uint32_t modifiers;
    bool precise_scrolling;
    double timestamp;
} GeoPointerEvent;

/// Counters for the debug overlay and for tests.
typedef struct GeoEngineStats {
    uint64_t frames_rendered;
    double last_frame_seconds;
} GeoEngineStats;

/// Returns the engine version as a static string.
const char* geo_engine_version(void);

/// Creates the engine and starts its thread. Returns null if this Mac's GPU is unsupported.
GeoEngine* geo_engine_create(void);
void geo_engine_destroy(GeoEngine* engine);

/// Hands the engine a CAMetalLayer. The caller configures its size; the engine retains it.
void geo_engine_attach_layer(GeoEngine* engine, void* metal_layer);

/// Notifies the engine that the viewport changed size, in drawable pixels.
void geo_engine_resize(GeoEngine* engine, uint32_t width, uint32_t height, float scale);

/// Posts pointer input. Never blocks; input is dropped only if the queue is full.
void geo_engine_post_pointer(GeoEngine* engine, const GeoPointerEvent* event);

/// Requests a redraw, for changes that do not arrive as input.
void geo_engine_request_frame(GeoEngine* engine);

GeoEngineStats geo_engine_stats(const GeoEngine* engine);

/// Prints current and peak memory footprint to stderr when GEOMETRY_TRACE is set.
void geo_trace_memory(const char* label);

#ifdef __cplusplus
}
#endif

#endif // GEOMETRY_ENGINE_H
