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
    kConstantShadowPass = 3,
    kConstantSection = 4,
    kConstantOccurrenceIds = 5,
    kConstantPeel = 6,
    kConstantShellLayer = 7,
    kConstantOutlines = 8,
};

struct Pipelines {
    // The last index of each of these is whether section planes are on.
    //
    // Opaque surfaces, by lighting: flat bands or the studio.
    id<MTLRenderPipelineState> surface[2][2];
    // Ghosts by lighting, then by whether they write pick ids, which they do while the ghost tool
    // is held so a click can bring one back.
    id<MTLRenderPipelineState> ghost[2][2][2];
    id<MTLRenderPipelineState> shadow[2]; // solid faces flattened onto the ground
    // See-through, by lighting where it matters: the shell's far walls, its nearest front faces
    // into depth, and those faces blended.
    id<MTLRenderPipelineState> shellBack[2][2];
    id<MTLRenderPipelineState> shellDepth[2];
    id<MTLRenderPipelineState> shellFront[2][2];
    // Occurrence ids for classifying the shell: the nearest part, then the nearest behind it.
    id<MTLRenderPipelineState> classify[2];
    id<MTLRenderPipelineState> sheet;
    // Feature edges into the ink attachment: creases and highlights, or ghosts' dashed lines.
    id<MTLRenderPipelineState> edges[2][2];
    id<MTLRenderPipelineState> cube, quad;
    // Inks the colour, then copies sample 0 of ids, depth and tags out of tile memory.
    id<MTLRenderPipelineState> resolveTile;
    // By whether the style draws outlines: without, the tag search and the fold are left out.
    id<MTLRenderPipelineState> composite[2];
    id<MTLComputePipelineState> ambientOcclusion;

    id<MTLDepthStencilState> depthWrite;  // reversed depth
    id<MTLDepthStencilState> depthTest;   // tests, writes nothing
    id<MTLDepthStencilState> farOnly;     // passes only where nothing was drawn
    id<MTLDepthStencilState> depthAlways; // passes, writes nothing
    id<MTLDepthStencilState> blendOnce;   // equal depth, first fragment per sample only

    /// Returns false, reporting why on stderr, if any pipeline fails to build.
    bool create(id<MTLDevice> device, id<MTLLibrary> library);
};

} // namespace geo::gpu
#endif
