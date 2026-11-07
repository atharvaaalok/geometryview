// The Metal 4 renderer. It owns all GPU objects and is used only from the engine thread.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "base/types.h"

#include <memory>

namespace geo::gpu {

class Renderer {
public:
    /// Returns null if the device cannot run the renderer, reporting why on stderr.
    static std::unique_ptr<Renderer> create(id<MTLDevice> device);
    ~Renderer();

    void attachLayer(CAMetalLayer* layer);
    void render(id<CAMetalDrawable> drawable);

private:
    Renderer();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace geo::gpu
#endif
