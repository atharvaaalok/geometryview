// What the engine asks of a GPU renderer, whatever API it draws with: Metal on the Mac, WebGPU in
// the universal viewer. Drawing a frame to the screen is not here, since each platform hands the
// renderer its own kind of drawable; everything the engine's logic needs is.
#pragma once

#include "base/types.h"
#include "model/face_state.h"
#include "render/overlay.h"
#include "render/view.h"
#include "scene/scene.h"
#include "shaders/shared/gpu_types.h"

#include <optional>
#include <span>
#include <vector>

namespace geo::render {

/// The rendered surface at one pixel.
struct PixelHit {
    u32 id;
    double viewDepth;
};

/// For classifying the shell: per pixel of a square view, the nearest occurrence and the nearest
/// other occurrence behind it, as global index plus one, or zero for none.
struct OccurrenceIds {
    std::vector<u32> front, behind;
};

class Backend {
public:
    virtual ~Backend() = default;

    /// Brings GPU tables up to date with `scene`, without waiting for the GPU.
    virtual void syncScene(const scene::Scene& scene) = 0;

    /// Uploads hidden, ghosted and selected flags if they changed since the last call.
    virtual void syncState(const model::FaceState& state) = 0;

    /// Sets the style for the frames that follow. `physical` chooses the studio light over flat
    /// bands.
    virtual void setStyle(const StyleUniforms& style, bool physical) = 0;

    /// Sets each occurrence's look, in global order, and the palette: three texels per look.
    virtual void syncLooks(std::span<const u16> lookOfOccurrence,
                           std::span<const u32> palette) = 0;

    /// While set, frames leave out feature edges and the cast shadow, for heavy scenes in motion.
    virtual void setReducedDetail(bool reduced) = 0;

    /// Triangles and edge segments drawn for the scene, counting every instance.
    virtual u64 sceneTriangles() const = 0;
    virtual u64 sceneEdgeSegments() const = 0;

    /// Allocates targets for this size, if it changed. Sizes are in pixels.
    virtual void resize(u32 width, u32 height) = 0;

    /// A glyph's advance in the overlay atlas, in atlas pixels.
    virtual float glyphAdvance() const = 0;

    /// The ids and depth the most recent frame drew at a pixel, or nothing outside the viewport or
    /// where nothing was drawn. The Metal renderer reads its own CPU-visible copy; a renderer that
    /// can only read back asynchronously answers with what its latest probe found.
    virtual std::optional<PixelHit> readPixel(int x, int y, const DepthDecoder& depth) const = 0;

    /// Renders the occurrence ids of a square view, `size` pixels wide, and waits for them.
    virtual OccurrenceIds renderOccurrenceIds(const ViewState& view, u32 size) = 0;

    /// Renders and returns BGRA pixels, for snapshots and tests. Blocks until done.
    virtual std::vector<u8> renderOffscreen(const ViewState& view,
                                            const Overlay* overlay = nullptr) = 0;
};

} // namespace geo::render
