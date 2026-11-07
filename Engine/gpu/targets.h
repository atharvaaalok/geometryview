// Render targets for one viewport size. Everything multisampled is memoryless, so it lives only in
// tile memory and costs no memory at all. What later passes or the CPU read is single-sampled.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include "base/types.h"
#include "gpu/gpu_context.h"

#include <vector>

namespace geo::gpu {

inline constexpr NSUInteger kSampleCount = 4;

// The main pass's attachments, which every pipeline drawn in it must declare alike.
inline constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
inline constexpr MTLPixelFormat kIdFormat = MTLPixelFormatR32Uint;
inline constexpr MTLPixelFormat kDepthCopyFormat = MTLPixelFormatR32Float;
inline constexpr MTLPixelFormat kInkFormat = MTLPixelFormatRGBA8Unorm;
inline constexpr MTLPixelFormat kTagFormat = MTLPixelFormatRG8Uint;
inline constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;

struct Targets {
    u32 width = 0, height = 0;
    id<MTLTexture> colorMsaa, idMsaa, depthCopyMsaa, inkMsaa, tagsMsaa, depthMsaa;
    id<MTLTexture> tags;           // sample 0 of the tags, for outlines
    id<MTLTexture> offscreenColor; // for renderOffscreen

    // Sample 0 of the ID and depth, in linear memory the CPU reads directly, so hover and pivots
    // need no GPU round trip.
    id<MTLBuffer> pickIdBuffer, pickDepthBuffer;
    id<MTLTexture> pickIds, pickDepths;
    NSUInteger pickIdRowBytes = 0, pickDepthRowBytes = 0;

    /// Allocates targets for a size, in pixels, replacing these in residency.
    void resize(const GpuContext& context, u32 newWidth, u32 newHeight);

    /// The main pass, built once per size. Each frame only changes the resolve target, so a frame
    /// allocates nothing.
    MTL4RenderPassDescriptor* mainPass(id<MTLTexture> resolveTarget);

    /// The composite pass, drawing over the main pass's result in `target`.
    MTL4RenderPassDescriptor* compositePass(id<MTLTexture> target);

private:
    std::vector<id<MTLAllocation>> resident() const;
    MTL4RenderPassDescriptor* mainPass_ = nil;
    MTL4RenderPassDescriptor* compositePass_ = nil;
};

} // namespace geo::gpu
#endif
