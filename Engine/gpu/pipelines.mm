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
    bool blended = false; // colour blends over what is there
    bool ids = true;      // pick id and depth copy
    bool ink = false;     // edge coverage, keeping the larger of what is there and what is drawn
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
    id<MTLRenderPipelineState> surface(bool ghost, bool physical, Writes writes) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&ghost type:MTLDataTypeBool atIndex:kConstantGhostPass];
        [constants setConstantValue:&physical type:MTLDataTypeBool atIndex:kConstantPhysical];
        MTL4MeshRenderPipelineDescriptor* mesh = [MTL4MeshRenderPipelineDescriptor new];
        mesh.objectFunctionDescriptor = function(@"surfaceObject", constants);
        mesh.meshFunctionDescriptor = function(@"surfaceMesh", constants);
        mesh.fragmentFunctionDescriptor = function(@"surfaceFragment", constants);
        mesh.payloadMemoryLength = kPayloadBytes;
        mesh.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        mesh.rasterSampleCount = kSampleCount;
        describeMainPass(mesh.colorAttachments, writes);
        return build(mesh, "Could not create a surface pipeline");
    }

    // Feature edges, as screen-space quads. The ghost-lines pass draws the dashed edges of ghosts;
    // the other draws creases and highlights.
    id<MTLRenderPipelineState> edges(bool ghostLines) {
        MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
        [constants setConstantValue:&ghostLines type:MTLDataTypeBool atIndex:kConstantGhostLines];
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
    // ink plus what remains of the colour beneath, so it blends as ink + colour * alpha.
    id<MTLRenderPipelineState> composite() {
        MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
        descriptor.vertexFunctionDescriptor = function(@"sheetVertex");
        descriptor.fragmentFunctionDescriptor = function(@"compositeFragment");
        MTL4RenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
        color.pixelFormat = kColorFormat;
        color.blendingState = MTL4BlendStateEnabled;
        color.sourceRGBBlendFactor = MTLBlendFactorOne;
        color.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
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
    for (const bool physical : {false, true}) {
        surface[physical] = builder.surface(false, physical, {.tags = MTLColorWriteMaskRed});
        ghost[physical][false] = builder.surface(
            true, physical, {.blended = true, .ids = false, .tags = MTLColorWriteMaskGreen});
        ghost[physical][true] = builder.surface(
            true, physical, {.blended = true, .ids = true, .tags = MTLColorWriteMaskGreen});
        if (!surface[physical] || !ghost[physical][false] || !ghost[physical][true]) {
            return false;
        }
    }
    sheet = builder.classic(@"sheetVertex", @"sheetFragment", {.ids = false},
                            "Could not create the sheet pipeline");
    for (const bool ghostLines : {false, true}) {
        edges[ghostLines] = builder.edges(ghostLines);
        applyInk[ghostLines] =
            builder.classic(@"sheetVertex", ghostLines ? @"applyGhostInk" : @"applyInk",
                            {.blended = true, .ids = false}, "Could not create an ink pipeline");
        if (!edges[ghostLines] || !applyInk[ghostLines]) {
            return false;
        }
    }
    // The cube writes pick ids, so it can be clicked; the scale bar's quads blend and do not.
    cube = builder.classic(@"cubeVertex", @"cubeFragment", {},
                           "Could not create the view cube pipeline");
    quad = builder.classic(@"quadVertex", @"quadFragment", {.blended = true, .ids = false},
                           "Could not create the overlay pipeline");
    resolvePick = builder.tile(@"resolvePick", "Could not create the pick pipeline");
    composite = builder.composite();
    ambientOcclusion =
        builder.compute(@"ambientOcclusion", "Could not create the occlusion kernel");
    if (!sheet || !cube || !quad || !resolvePick || !composite || !ambientOcclusion) {
        return false;
    }

    depthWrite = depthState(device, MTLCompareFunctionGreater, true);
    depthTest = depthState(device, MTLCompareFunctionGreater, false);
    farOnly = depthState(device, MTLCompareFunctionGreaterEqual, false);
    depthAlways = depthState(device, MTLCompareFunctionAlways, false);
    return true;
}

} // namespace geo::gpu
