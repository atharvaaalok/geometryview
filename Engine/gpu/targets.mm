#include "gpu/targets.h"

#include "shaders/shared/gpu_types.h"

namespace geo::gpu {
namespace {

id<MTLTexture> makeTarget(id<MTLDevice> device, u32 width, u32 height, MTLPixelFormat format,
                          NSUInteger samples, MTLStorageMode storage, MTLTextureUsage usage) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.sampleCount = samples;
    d.storageMode = storage;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

// A texture over a shared buffer, so the CPU reads its rows in place.
void makeLinear(const GpuContext& context, u32 width, u32 height, MTLPixelFormat format,
                id<MTLBuffer> __strong& buffer, id<MTLTexture> __strong& texture,
                NSUInteger& rowBytes) {
    const NSUInteger alignment =
        [context.device minimumLinearTextureAlignmentForPixelFormat:format];
    rowBytes = (NSUInteger(width) * 4 + alignment - 1) & ~(alignment - 1);
    buffer = context.makeShared(rowBytes * height);
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    d.resourceOptions = buffer.resourceOptions;
    texture = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:rowBytes];
}

} // namespace

std::vector<id<MTLAllocation>> Targets::resident() const {
    if (pickIdBuffer == nil) {
        return {};
    }
    return {pickIdBuffer, pickDepthBuffer, offscreenColor, tags, occlusion, shadowMask};
}

void Targets::resize(const GpuContext& context, u32 newWidth, u32 newHeight) {
    if (newWidth == width && newHeight == height) {
        return;
    }
    for (id<MTLAllocation> allocation : resident()) {
        [context.residency removeAllocation:allocation];
    }
    width = newWidth;
    height = newHeight;
    mainPass_ = nil;
    compositePass_ = nil;
    shadowPass_ = nil;

    id<MTLDevice> device = context.device;
    const MTLTextureUsage target = MTLTextureUsageRenderTarget;
    colorMsaa = makeTarget(device, width, height, kColorFormat, kSampleCount,
                           MTLStorageModeMemoryless, target);
    idMsaa = makeTarget(device, width, height, kIdFormat, kSampleCount, MTLStorageModeMemoryless,
                        target);
    depthCopyMsaa = makeTarget(device, width, height, kDepthCopyFormat, kSampleCount,
                               MTLStorageModeMemoryless, target);
    inkMsaa = makeTarget(device, width, height, kInkFormat, kSampleCount, MTLStorageModeMemoryless,
                         target);
    tagsMsaa = makeTarget(device, width, height, kTagFormat, kSampleCount, MTLStorageModeMemoryless,
                          target);
    tags = makeTarget(device, width, height, kTagFormat, 1, MTLStorageModePrivate,
                      MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    occlusion =
        makeTarget(device, (width + 1) / 2, (height + 1) / 2, MTLPixelFormatR8Unorm, 1,
                   MTLStorageModePrivate, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    shadowMask =
        makeTarget(device, (width + 1) / 2, (height + 1) / 2, kShadowFormat, 1,
                   MTLStorageModePrivate, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    depthMsaa = makeTarget(device, width, height, kDepthFormat, kSampleCount,
                           MTLStorageModeMemoryless, target);
    offscreenColor =
        makeTarget(device, width, height, kColorFormat, 1, MTLStorageModeShared, target);
    makeLinear(context, width, height, kIdFormat, pickIdBuffer, pickIds, pickIdRowBytes);
    makeLinear(context, width, height, kDepthCopyFormat, pickDepthBuffer, pickDepths,
               pickDepthRowBytes);

    for (id<MTLAllocation> allocation : resident()) {
        [context.residency addAllocation:allocation];
    }
    [context.residency commit];
    [context.residency requestResidency];
}

MTL4RenderPassDescriptor* Targets::shadowPass() {
    if (shadowPass_ == nil) {
        shadowPass_ = [MTL4RenderPassDescriptor new];
        shadowPass_.colorAttachments[0].texture = shadowMask;
        shadowPass_.colorAttachments[0].loadAction = MTLLoadActionClear;
        shadowPass_.colorAttachments[0].storeAction = MTLStoreActionStore;
        shadowPass_.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    return shadowPass_;
}

MTL4RenderPassDescriptor* Targets::compositePass(id<MTLTexture> target) {
    if (compositePass_ == nil) {
        compositePass_ = [MTL4RenderPassDescriptor new];
        compositePass_.colorAttachments[0].loadAction = MTLLoadActionLoad;
        compositePass_.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    compositePass_.colorAttachments[0].texture = target;
    return compositePass_;
}

MTL4RenderPassDescriptor* Targets::mainPass(id<MTLTexture> resolveTarget) {
    if (mainPass_ != nil) {
        mainPass_.colorAttachments[kAttachmentColor].resolveTexture = resolveTarget;
        return mainPass_;
    }
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    const auto attach = [&](NSUInteger index, id<MTLTexture> texture) {
        MTLRenderPassColorAttachmentDescriptor* a = pass.colorAttachments[index];
        a.texture = texture;
        a.loadAction = MTLLoadActionClear;
        a.storeAction = MTLStoreActionDontCare;
        a.clearColor = MTLClearColorMake(0, 0, 0, 0);
    };
    attach(kAttachmentColor, colorMsaa);
    pass.colorAttachments[kAttachmentColor].resolveTexture = resolveTarget;
    pass.colorAttachments[kAttachmentColor].storeAction = MTLStoreActionMultisampleResolve;
    attach(kAttachmentId, idMsaa);
    attach(kAttachmentDepth, depthCopyMsaa);
    attach(kAttachmentInk, inkMsaa);
    attach(kAttachmentTags, tagsMsaa);
    pass.depthAttachment.texture = depthMsaa;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 0.0; // reversed depth
    pass.stencilAttachment.texture = depthMsaa;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
    pass.stencilAttachment.clearStencil = 0;
    pass.tileWidth = kTileSize;
    pass.tileHeight = kTileSize;
    mainPass_ = pass;
    return pass;
}

} // namespace geo::gpu
