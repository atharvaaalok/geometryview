// Every pipeline the renderer draws with, built up front so no frame waits for a shader compile.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace geo::gpu {

/// Function constant indices, shared with the shaders.
enum FunctionConstant : NSUInteger {
    kConstantGhostPass = 0,
    kConstantPhysical = 1,
    kConstantGhostLines = 2,
};

struct Pipelines {
    // Opaque surfaces, by lighting: flat bands or the studio.
    id<MTLRenderPipelineState> surface[2];
    // Ghosts by lighting, then by whether they write pick ids, which they do while the ghost tool
    // is held so a click can bring one back.
    id<MTLRenderPipelineState> ghost[2][2];
    id<MTLRenderPipelineState> sheet;
    // Feature edges into the ink attachment, then ink onto colour: creases and highlights, or
    // ghosts' dashed lines.
    id<MTLRenderPipelineState> edges[2];
    id<MTLRenderPipelineState> applyInk[2];
    id<MTLRenderPipelineState> cube, quad;
    id<MTLRenderPipelineState> resolvePick;
    id<MTLRenderPipelineState> composite;
    id<MTLComputePipelineState> ambientOcclusion;

    id<MTLDepthStencilState> depthWrite;  // reversed depth
    id<MTLDepthStencilState> depthTest;   // tests, writes nothing
    id<MTLDepthStencilState> farOnly;     // passes only where nothing was drawn
    id<MTLDepthStencilState> depthAlways; // passes, writes nothing

    /// Returns false, reporting why on stderr, if any pipeline fails to build.
    bool create(id<MTLDevice> device, id<MTLLibrary> library);
};

} // namespace geo::gpu
#endif
