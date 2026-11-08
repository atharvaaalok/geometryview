// The WGSL sources, embedded at build time from Engine/webgpu/shaders. Each pass's module is the
// common source followed by its own.
#pragma once

namespace geo::webgpu::wgsl {

extern const char* const kCommon;
extern const char* const kCull;
extern const char* const kSurface;
extern const char* const kEdges;
extern const char* const kSheet;
extern const char* const kPost;
extern const char* const kOverlay;

} // namespace geo::webgpu::wgsl
