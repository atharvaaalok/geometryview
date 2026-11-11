#include "gpu/pipelines.h"

#include "gpu/targets.h"
#include "shaders/shared/gpu_types.h"

#include <cstdio>

namespace geo::gpu {
namespace {

constexpr NSUInteger kPayloadBytes = sizeof(uint32_t) * (kBatchMeshlets + 1);

void reportFailure(const char* what, NSError* error) {
    std::fprintf(stderr, "%s: %s\n", what,
                 error ? error.localizedDescription.UTF8String : "unknown error");
}

// What a pipeline in the main pass writes.
struct Writes {
    bool color = true;
    bool blended = false;   // colour blends over what is there
    bool ids = true;        // pick id and depth copy
    bool ink = false;       // edge coverage, keeping the larger of what is there and what is drawn
    bool clearsInk = false; // takes away ink beneath, by the colour's alpha
    bool parity = false;    // the section's per-part count of surfaces behind the cut
    MTLColorWriteMask tags = MTLColorWriteMaskNone; // red for solid bodies, green for ghosts
};

void describeMainPass(MTL4RenderPipelineColorAttachmentDescriptorArray* attachments,
                      Writes writes) {
    attachments[kAttachmentColor].pixelFormat = kColorFormat;
    attachments[kAttachmentId].pixelFormat = kIdFormat;
    attachments[kAttachmentDepth].pixelFormat = kDepthCopyFormat;
    attachments[kAttachmentInk].pixelFormat = kInkFormat;
    attachments[kAttachmentTags].pixelFormat = kTagFormat;
    attachments[kAttachmentTags].writeMask = writes.tags;
    attachments[kAttachmentParity].pixelFormat = kParityFormat;
    if (!writes.parity) {
        attachments[kAttachmentParity].writeMask = MTLColorWriteMaskNone;
    }
    if (!writes.color) {
        attachments[kAttachmentColor].writeMask = MTLColorWriteMaskNone;
    }
    if (writes.ink) {
        MTL4RenderPipelineColorAttachmentDescriptor* ink = attachments[kAttachmentInk];
        ink.blendingState = MTL4BlendStateEnabled;
        ink.rgbBlendOperation = MTLBlendOperationMax;
        ink.alphaBlendOperation = MTLBlendOperationMax;
        ink.sourceRGBBlendFactor = MTLBlendFactorOne;
        ink.destinationRGBBlendFactor = MTLBlendFactorOne;
        ink.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ink.destinationAlphaBlendFactor = MTLBlendFactorOne;
    } else if (writes.clearsInk) {
        MTL4RenderPipelineColorAttachmentDescriptor* ink = attachments[kAttachmentInk];
        ink.blendingState = MTL4BlendStateEnabled;
        ink.sourceRGBBlendFactor = MTLBlendFactorZero;
        ink.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ink.sourceAlphaBlendFactor = MTLBlendFactorZero;
        ink.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else {
        attachments[kAttachmentInk].writeMask = MTLColorWriteMaskNone;
    }
    if (writes.blended) {
        MTL4RenderPipelineColorAttachmentDescriptor* color = attachments[kAttachmentColor];
        color.blendingState = MTL4BlendStateEnabled;
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    if (!writes.ids) {
        attachments[kAttachmentId].writeMask = MTLColorWriteMaskNone;
        attachments[kAttachmentDepth].writeMask = MTLColorWriteMaskNone;
    }
}

class Builder {
public:
    Builder(id<MTLDevice> device, id<MTLLibrary> library) : library_(library) {
        NSError* error = nil;
        compiler_ = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        if (!compiler_) {
            reportFailure("Could not create the shader compiler", error);
        }
    }

    bool ok() const { return compiler_ != nil; }

    MTL4FunctionDescriptor* function(NSString* name, MTLFunctionConstantValues* constants = nil) {
        MTL4LibraryFunctionDescriptor* f = [MTL4LibraryFunctionDescriptor new];
        f.library = library_;
        f.name = name;
        if (constants == nil) {
            return f;
        }
        MTL4SpecializedFunctionDescriptor* specialized = [MTL4SpecializedFunctionDescriptor new];
        specialized.functionDescriptor = f;
        specialized.constantValues = constants;
        return specialized;
    }

    id<MTLRenderPipelineState> build(MTL4PipelineDescriptor* descriptor, const char* what) {
        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [compiler_ newRenderPipelineStateWithDescriptor:descriptor
                                        compilerTaskOptions:nil
                                                      error:&error];
        if (!state) {
            reportFailure(what, error);
        }
        return state;
    }

    // One function set serves every surface pass. Function constants choose the pass and the
    // lighting, so each pipeline is specialised and carries no branches for the others.
    struct SurfaceKind {
        bool ghost = false;
        bool physical = false;
        bool shadow = false;
        bool section = false;
        bool occurrenceIds = false;
        bool peel = false;
        uint32_t shellLayer = kShellNone;
    };

    MTLFunctionConstantValues* surfaceConstants(SurfaceKind kind) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&kind.ghost type:MTLDataTypeBool atIndex:kConstantGhostPass];
        [constants setConstantValue:&kind.physical type:MTLDataTypeBool atIndex:kConstantPhysical];
        [constants setConstantValue:&kind.shadow type:MTLDataTypeBool atIndex:kConstantShadowPass];
        [constants setConstantValue:&kind.section type:MTLDataTypeBool atIndex:kConstantSection];
        [constants setConstantValue:&kind.occurrenceIds
                               type:MTLDataTypeBool
                            atIndex:kConstantOccurrenceIds];
        [constants setConstantValue:&kind.peel type:MTLDataTypeBool atIndex:kConstantPeel];
        [constants setConstantValue:&kind.shellLayer
                               type:MTLDataTypeUInt
                            atIndex:kConstantShellLayer];
        return constants;
    }

    // Any surface pass in the main pass, or at `samples` for the classification views.
    id<MTLRenderPipelineState> surfaceKind(SurfaceKind kind, Writes writes,
                                           NSUInteger samples = kSampleCount,
                                           NSString* fragment = @"surfaceFragment") {
        MTLFunctionConstantValues* constants = surfaceConstants(kind);
        MTL4MeshRenderPipelineDescriptor* mesh = [MTL4MeshRenderPipelineDescriptor new];
        mesh.objectFunctionDescriptor = function(@"surfaceObject", constants);
        mesh.meshFunctionDescriptor = function(@"surfaceMesh", constants);
        mesh.fragmentFunctionDescriptor = function(fragment, constants);
        mesh.payloadMemoryLength = kPayloadBytes;
        mesh.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        mesh.rasterSampleCount = samples;
        describeMainPass(mesh.colorAttachments, writes);
        return build(mesh, "Could not create a surface pipeline");
    }

    // The shadow mask: surfaces flattened onto the ground, coverage only, at half resolution.
    id<MTLRenderPipelineState> shadow(bool section) {
        MTLFunctionConstantValues* constants =
            surfaceConstants({.shadow = true, .section = section});
        MTL4MeshRenderPipelineDescriptor* mesh = [MTL4MeshRenderPipelineDescriptor new];
        mesh.objectFunctionDescriptor = function(@"surfaceObject", constants);
        mesh.meshFunctionDescriptor = function(@"surfaceMesh", constants);
        mesh.fragmentFunctionDescriptor = function(@"shadowFragment", constants);
        mesh.payloadMemoryLength = kPayloadBytes;
        mesh.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        mesh.colorAttachments[0].pixelFormat = kShadowFormat;
        return build(mesh, "Could not create the shadow pipeline");
    }

    id<MTLRenderPipelineState> surface(bool ghost, bool physical, bool section, Writes writes) {
        return surfaceKind({.ghost = ghost, .physical = physical, .section = section}, writes);
    }

    // Feature edges, as screen-space quads. The ghost-lines pass draws the dashed edges of ghosts;
    // the other draws creases and highlights.
    id<MTLRenderPipelineState> edges(bool ghostLines, bool section) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&ghostLines type:MTLDataTypeBool atIndex:kConstantGhostLines];
        [constants setConstantValue:&section type:MTLDataTypeBool atIndex:kConstantSection];
        MTL4MeshRenderPipelineDescriptor* mesh = [MTL4MeshRenderPipelineDescriptor new];
        mesh.objectFunctionDescriptor = function(@"edgeObject", constants);
        mesh.meshFunctionDescriptor = function(@"edgeMesh", constants);
        mesh.fragmentFunctionDescriptor = function(@"edgeFragment", constants);
        mesh.payloadMemoryLength = kPayloadBytes;
        mesh.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        mesh.rasterSampleCount = kSampleCount;
        describeMainPass(mesh.colorAttachments, {.color = false, .ids = false, .ink = true});
        return build(mesh, "Could not create an edge pipeline");
    }

    id<MTLRenderPipelineState> classic(NSString* vertex, NSString* fragment, Writes writes,
                                       const char* what) {
        MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
        descriptor.vertexFunctionDescriptor = function(vertex);
        descriptor.fragmentFunctionDescriptor = function(fragment);
        descriptor.rasterSampleCount = kSampleCount;
        describeMainPass(descriptor.colorAttachments, writes);
        return build(descriptor, what);
    }

    // Outlines and print texture, over the resolved image: each pixel's result is premultiplied
    // ink plus the colour beneath times the second source, what remains of it. Alpha blends the
    // same way, so a frame on a clear sheet comes out premultiplied.
    id<MTLRenderPipelineState> composite(bool outlines) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&outlines type:MTLDataTypeBool atIndex:kConstantOutlines];
        MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
        descriptor.vertexFunctionDescriptor = function(@"sheetVertex");
        descriptor.fragmentFunctionDescriptor = function(@"compositeFragment", constants);
        MTL4RenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
        color.pixelFormat = kColorFormat;
        color.blendingState = MTL4BlendStateEnabled;
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorSource1Color;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorSource1Alpha;
        return build(descriptor, "Could not create the composite pipeline");
    }

    id<MTLComputePipelineState> compute(NSString* name, const char* what) {
        MTL4ComputePipelineDescriptor* descriptor = [MTL4ComputePipelineDescriptor new];
        descriptor.computeFunctionDescriptor = function(name);
        NSError* error = nil;
        id<MTLComputePipelineState> state =
            [compiler_ newComputePipelineStateWithDescriptor:descriptor
                                         compilerTaskOptions:nil
                                                       error:&error];
        if (!state) {
            reportFailure(what, error);
        }
        return state;
    }

    id<MTLRenderPipelineState> tile(NSString* name, const char* what) {
        MTL4TileRenderPipelineDescriptor* tile = [MTL4TileRenderPipelineDescriptor new];
        tile.tileFunctionDescriptor = function(name);
        tile.rasterSampleCount = kSampleCount;
        tile.threadgroupSizeMatchesTileSize = YES;
        tile.colorAttachments[kAttachmentColor].pixelFormat = kColorFormat;
        tile.colorAttachments[kAttachmentId].pixelFormat = kIdFormat;
        tile.colorAttachments[kAttachmentDepth].pixelFormat = kDepthCopyFormat;
        tile.colorAttachments[kAttachmentInk].pixelFormat = kInkFormat;
        tile.colorAttachments[kAttachmentTags].pixelFormat = kTagFormat;
        tile.colorAttachments[kAttachmentParity].pixelFormat = kParityFormat;
        return build(tile, what);
    }

private:
    id<MTLLibrary> library_;
    id<MTL4Compiler> compiler_;
};

id<MTLDepthStencilState> depthState(id<MTLDevice> device, MTLCompareFunction compare, bool write) {
    MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = compare;
    depth.depthWriteEnabled = write;
    return [device newDepthStencilStateWithDescriptor:depth];
}

} // namespace

bool Pipelines::create(id<MTLDevice> device, id<MTLLibrary> library) {
    Builder builder(device, library);
    if (!builder.ok()) {
        return false;
    }
    for (const bool section : {false, true}) {
        for (const bool physical : {false, true}) {
            surface[physical][section] =
                builder.surface(false, physical, section, {.tags = MTLColorWriteMaskRed});
            for (const bool pickable : {false, true}) {
                ghost[physical][pickable][section] = builder.surface(
                    true, physical, section,
                    {.blended = true, .ids = pickable, .tags = MTLColorWriteMaskGreen});
                if (!ghost[physical][pickable][section]) {
                    return false;
                }
            }
            if (!surface[physical][section]) {
                return false;
            }
        }
        shadow[section] = builder.shadow(section);
        // See-through: far walls opaque and pinned behind everything, then the nearest front
        // faces into depth, then blended once.
        shellDepth[section] = builder.surfaceKind({.section = section, .shellLayer = kShellDepth},
                                                  {.color = false, .ids = false});
        for (const bool physical : {false, true}) {
            shellBack[physical][section] = builder.surfaceKind(
                {.physical = physical, .section = section, .shellLayer = kShellBack}, {});
            shellFront[physical][section] = builder.surfaceKind(
                {.physical = physical, .section = section, .shellLayer = kShellFront},
                {.blended = true, .ids = false, .tags = MTLColorWriteMaskGreen});
            if (!shellBack[physical][section] || !shellFront[physical][section]) {
                return false;
            }
        }
        if (!shellDepth[section]) {
            return false;
        }
    }
    // Caps that touching parts cannot break: each part's count of the surfaces behind the cut,
    // then its nearest back face drawn on the plane where its count is odd.
    parity = builder.surfaceKind({.section = true}, {.color = false, .ids = false, .parity = true},
                                 kSampleCount, @"parityFragment");
    for (const bool physical : {false, true}) {
        cap[physical] =
            builder.surfaceKind({.physical = physical, .section = true},
                                {.tags = MTLColorWriteMaskRed}, kSampleCount, @"capFragment");
        if (!cap[physical]) {
            return false;
        }
    }
    if (!parity) {
        return false;
    }
    for (const bool section : {false, true}) {
        for (const bool ghostLines : {false, true}) {
            edges[ghostLines][section] = builder.edges(ghostLines, section);
            if (!edges[ghostLines][section]) {
                return false;
            }
        }
    }
    sheet = builder.classic(@"sheetVertex", @"sheetFragment", {.ids = false},
                            "Could not create the sheet pipeline");
    // The cube writes pick ids, so it can be clicked; the scale bar's quads blend and do not.
    cube =
        builder.classic(@"cubeVertex", @"cubeFragment",
                        {.clearsInk = true, .tags = MTLColorWriteMaskRed | MTLColorWriteMaskGreen},
                        "Could not create the view cube pipeline");
    quad = builder.classic(@"quadVertex", @"quadFragment",
                           {.blended = true, .ids = false, .clearsInk = true},
                           "Could not create the overlay pipeline");
    resolveTile = builder.tile(@"resolveTile", "Could not create the tile resolve pipeline");
    composite[0] = builder.composite(false);
    composite[1] = builder.composite(true);
    ambientOcclusion =
        builder.compute(@"ambientOcclusion", "Could not create the occlusion kernel");
    if (!shadow[0] || !shadow[1] || !sheet || !cube || !quad || !resolveTile || !composite[0] || !composite[1] ||
        !ambientOcclusion) {
        return false;
    }

    for (const bool peel : {false, true}) {
        classify[peel] = builder.surfaceKind({.occurrenceIds = true, .peel = peel}, {}, 1);
        if (!classify[peel]) {
            return false;
        }
    }

    // The shell's front faces blend once per sample, even where two lie at the same depth: the
    // first to pass marks the stencil, and the rest fail it.
    MTLDepthStencilDescriptor* once = [MTLDepthStencilDescriptor new];
    once.depthCompareFunction = MTLCompareFunctionEqual;
    once.depthWriteEnabled = NO;
    MTLStencilDescriptor* stencil = [MTLStencilDescriptor new];
    stencil.stencilCompareFunction = MTLCompareFunctionEqual;
    stencil.depthStencilPassOperation = MTLStencilOperationIncrementClamp;
    once.frontFaceStencil = stencil;
    once.backFaceStencil = stencil;
    blendOnce = [device newDepthStencilStateWithDescriptor:once];

    depthWrite = depthState(device, MTLCompareFunctionGreater, true);
    depthTest = depthState(device, MTLCompareFunctionGreater, false);
    farOnly = depthState(device, MTLCompareFunctionGreaterEqual, false);
    depthAlways = depthState(device, MTLCompareFunctionAlways, false);
    return true;
}

} // namespace geo::gpu
