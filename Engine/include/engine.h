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

/// Returns the engine version as a static string.
const char* geo_engine_version(void);

#ifdef __cplusplus
}
#endif

#endif // GEOMETRY_ENGINE_H
