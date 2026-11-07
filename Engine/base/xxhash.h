// xxHash, configured for every platform this project builds for. Include this rather than
// <xxhash.h> directly.
#pragma once

#if defined(__wasm_simd128__) && !defined(XXH_HAS_INCLUDE)
// xxHash would reach SIMD128 through Emscripten's NEON emulation, which it includes inside an
// extern "C" block, where that header does not compile. Without it, xxHash runs its scalar path.
#define XXH_HAS_INCLUDE(header) 0
#endif

#include <xxhash.h>
