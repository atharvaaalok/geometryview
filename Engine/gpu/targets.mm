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
    return {pickIdBuffer, pickDepthBuffer, offscreenColor};
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
    pass.depthAttachment.texture = depthMsaa;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 0.0; // reversed depth
    pass.tileWidth = kTileSize;
    pass.tileHeight = kTileSize;
    mainPass_ = pass;
    return pass;
}

} // namespace geo::gpu
