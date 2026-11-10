// The Metal 4 renderer. It owns all GPU objects and is used only from the engine thread.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "base/types.h"
#include "math/geometry.h"
#include "render/backend.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace geo::gpu {

using render::OccurrenceIds;
using render::PixelHit;

class Renderer final : public render::Backend {
public:
    /// Returns null if the device cannot run the renderer, reporting why on stderr.
    static std::unique_ptr<Renderer> create(id<MTLDevice> device, const char* libraryPath);
    ~Renderer() override;

    void attachLayer(CAMetalLayer* layer);

    /// Brings GPU tables up to date with `scene`. Never waits for the GPU: tables a frame may still
    /// be reading stay alive until that frame finishes, and mapped geometry stays alive with them.
    void syncScene(const scene::Scene& scene) override;

    /// Uploads hidden, ghosted and selected flags if they changed since the last call.
    void syncState(const model::FaceState& state) override;

    /// Sets the style for the frames that follow. `physical` chooses the studio light over flat
    /// bands.
    void setStyle(const StyleUniforms& style, bool physical) override;

    /// Sets each occurrence's look, in global order, and the palette: three texels per look.
    void syncLooks(std::span<const u16> lookOfOccurrence, std::span<const u32> palette) override;
    void setShellGroups(std::span<const u32> groupOfOccurrence) override;

    /// Renders the occurrence ids of a square view, `size` pixels wide, and waits for them.
    OccurrenceIds renderOccurrenceIds(const render::ViewState& view, u32 size) override;

    /// While set, frames leave out feature edges and the cast shadow, for heavy scenes in motion.
    void setReducedDetail(bool reduced) override;

    /// Triangles and edge segments drawn for the scene, counting every instance.
    u64 sceneTriangles() const override;
    u64 sceneEdgeSegments() const override;

    /// Allocates targets for this size, if it changed. Sizes are in pixels.
    void resize(u32 width, u32 height) override;

    /// Draws a frame. The overlay, if given, is drawn over the model.
    void render(id<CAMetalDrawable> drawable, const render::ViewState& view,
                const render::Overlay* overlay = nullptr);

    /// Renders and returns BGRA pixels, for tests and `gcli render`. Blocks until done.
    std::vector<u8> renderOffscreen(const render::ViewState& view,
                                    const render::Overlay* overlay = nullptr) override;

    /// Mean GPU time per frame so far, from Metal's commit feedback, which is gathered only when
    /// tracing. Safe to call from any thread.
    double meanGpuSeconds() const;
    /// The given fraction's percentile of the latest 2048 frames' GPU times, likewise.
    double gpuPercentileSeconds(double fraction) const;

    /// A glyph's advance in the overlay atlas, in atlas pixels.
    float glyphAdvance() const override;

    /// Reads the most recent frame's ID and depth from CPU-visible memory. No GPU round trip.
    /// Pixels outside the viewport, or with nothing drawn, return nothing.
    std::optional<PixelHit> readPixel(int x, int y, const render::DepthDecoder& depth) const override;

private:
    Renderer();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace geo::gpu
#endif
