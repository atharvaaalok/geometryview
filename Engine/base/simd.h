// Vectors, matrices and quaternions. Apple platforms use <simd/simd.h>; everywhere else, such as
// WebAssembly, simd_portable.h provides the same names with the same layout and results.
#pragma once

#if defined(__APPLE__) && !defined(GEO_PORTABLE_SIMD)
#include <simd/simd.h>
#else
#include "base/simd_portable.h"
#endif
