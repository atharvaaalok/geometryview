#include "gpu/renderer.h"

#include "base/assert.h"

#include <array>
#include <cstdio>

namespace geo::gpu {
namespace {

// With two drawables, a third frame cannot start until an earlier one finishes, so two allocators
// are enough: reusing one only waits for the frame that last used it.
constexpr usize kFramesInFlight = 2;

// Paper, #f2efe7, written as encoded sRGB because colours composite in encoded space.
const MTLClearColor kPaper = MTLClearColorMake(242.0 / 255.0, 239.0 / 255.0, 231.0 / 255.0, 1.0);

} // namespace

struct Renderer::State {
    id<MTLDevice> device;
    id<MTL4CommandQueue> queue;
    id<MTL4CommandBuffer> commandBuffer;
    std::array<id<MTL4CommandAllocator>, kFramesInFlight> allocators;
    id<MTLSharedEvent> frameFinished;
    u64 frameCount = 0;
};

Renderer::Renderer() : state_(std::make_unique<State>()) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::create(id<MTLDevice> device) {
    if (![device supportsFamily:MTLGPUFamilyMetal4]) {
        std::fprintf(stderr, "Geometry needs a GPU that supports Metal 4.\n");
        return nullptr;
    }
    std::unique_ptr<Renderer> renderer(new Renderer);
    State& s = *renderer->state_;
    s.device = device;
    s.queue = [device newMTL4CommandQueue];
    s.commandBuffer = [device newCommandBuffer];
    s.frameFinished = [device newSharedEvent];
    for (usize i = 0; i < kFramesInFlight; ++i) {
        s.allocators[i] = [device newCommandAllocator];
    }
    return renderer;
}

void Renderer::attachLayer(CAMetalLayer* layer) {
    [state_->queue addResidencySet:layer.residencySet];
}

void Renderer::render(id<CAMetalDrawable> drawable) {
    State& s = *state_;

    // Reusing an allocator requires the frame that last used it to have finished.
    const u64 frame = ++s.frameCount;
    if (frame > kFramesInFlight) {
        constexpr u64 kTimeoutMs = 1000;
        const bool finished = [s.frameFinished waitUntilSignaledValue:frame - kFramesInFlight
                                                            timeoutMS:kTimeoutMs];
        GEO_ASSERT(finished);
    }
    id<MTL4CommandAllocator> allocator = s.allocators[frame % kFramesInFlight];
    [allocator reset];

    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = kPaper;

    [s.commandBuffer beginCommandBufferWithAllocator:allocator];
    id<MTL4RenderCommandEncoder> encoder = [s.commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [s.commandBuffer endCommandBuffer];

    id<MTL4CommandBuffer> buffers[] = {s.commandBuffer};
    [s.queue waitForDrawable:drawable];
    [s.queue commit:buffers count:1];
    [s.queue signalDrawable:drawable];
    [s.queue signalEvent:s.frameFinished value:frame];
    [drawable present];
}

} // namespace geo::gpu
