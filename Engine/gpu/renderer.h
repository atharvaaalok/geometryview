// The Metal 4 renderer. It owns all GPU objects and is used only from the engine thread.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "base/types.h"
#include "math/geometry.h"
#include "render/view.h"
#include "scene/mapped_blob.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace geo::gpu {

/// The rendered surface at one pixel.
struct PixelHit {
    u32 id;
    double viewDepth;
};

class Renderer {
public:
    /// Returns null if the device cannot run the renderer, reporting why on stderr.
    static std::unique_ptr<Renderer> create(id<MTLDevice> device, const char* libraryPath);
    ~Renderer();

    void attachLayer(CAMetalLayer* layer);

    /// Takes ownership of the mapping, so geometry stays valid until the GPU is done with it.
    void setScene(scene::MappedBlob& blob);
    bool hasScene() const;
    Aabb sceneBounds() const;

    /// Allocates targets for this size, if it changed. Sizes are in pixels.
    void resize(u32 width, u32 height);

    void render(id<CAMetalDrawable> drawable, const render::ViewState& view);

    /// Renders and returns BGRA pixels, for tests and `gcli render`. Blocks until done.
    std::vector<u8> renderOffscreen(const render::ViewState& view);

    /// Reads the most recent frame's ID and depth from CPU-visible memory. No GPU round trip.
    /// Pixels outside the viewport, or with nothing drawn, return nothing.
    std::optional<PixelHit> readPixel(int x, int y, const render::DepthDecoder& depth) const;

private:
    Renderer();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace geo::gpu
#endif
