// The WebGPU renderer: the Metal renderer's frames, drawn from WGSL ports of its shaders, for the
// universal viewer. It runs natively on Dawn, which tests it against Metal, and in the browser.
//
// Where Metal has mesh shaders, a compute pass culls meshlets and writes the visible ones' indices,
// and one indexed draw per list pulls their vertices from the cache's own records. Where Metal has
// four samples in tile memory, the main pass has one, and antialiasing comes from jittered passes
// at the same four sample positions, accumulated. See ADR 0014.
#pragma once

#include "render/backend.h"
#include "scene/segment.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <memory>
#include <string>

namespace geo::webgpu {

/// Also a SegmentSink: a cache file streamed into it keeps its segments' bytes on the GPU only.
class Renderer final : public render::Backend, public scene::SegmentSink {
public:
    /// Frames are drawn into textures of `outputFormat`. `instance` is waited on by the calls that
    /// block (offscreen renders and classifying the shell), which only native hosts may make. The
    /// device should be created with the adapter's buffer size limits.
    static std::unique_ptr<Renderer> create(wgpu::Instance instance, wgpu::Device device,
                                            wgpu::TextureFormat outputFormat, std::string& error);
    ~Renderer() override;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void syncScene(const scene::Scene& scene) override;
    void syncState(const model::FaceState& state) override;
    void setStyle(const StyleUniforms& style, bool physical) override;
    void syncLooks(std::span<const u16> lookOfOccurrence, std::span<const u32> palette) override;
    void setReducedDetail(bool reduced) override;
    u64 sceneTriangles() const override;
    u64 sceneEdgeSegments() const override;
    void resize(u32 width, u32 height) override;
    float glyphAdvance() const override;
    std::optional<render::PixelHit> readPixel(int x, int y,
                                              const render::DepthDecoder& depth) const override;
    render::OccurrenceIds renderOccurrenceIds(const render::ViewState& view, u32 size) override;
    void requestOccurrenceIds(const render::ViewState& view, u32 size,
                              std::function<void(render::OccurrenceIds)> done) override;
    std::vector<u8> renderOffscreen(const render::ViewState& view,
                                    const render::Overlay* overlay = nullptr) override;

    std::shared_ptr<scene::Placement> place(u64 size) override;
    void write(scene::Placement& placement, u64 offset, std::span<const u8> bytes) override;
    /// Makes room for a streamed file's geometry at once, so the buffer is not grown, and
    /// copied, segment by segment.
    void expect(u64 size) override;

    /// Draws a frame into `target`, which must be the size last given to `resize`.
    void render(const wgpu::Texture& target, const render::ViewState& view,
                const render::Overlay* overlay = nullptr);

    /// Jittered samples per frame, from 1 to 4. Offscreen renders always take 4.
    void setSamples(u32 samples);

    /// The overlay's text: coverage, one byte per pixel, kAtlasWidth by kAtlasHeight, as the
    /// host draws it, and a glyph's advance in atlas pixels. Until it is given, no overlay draws.
    void setOverlayAtlas(std::span<const u8> pixels, float glyphAdvance);

    /// The GPU time of the latest frame timed, from timestamps, where the device has them, and
    /// how many frames have been timed so far.
    double gpuMilliseconds() const;
    u64 timedFrames() const;
    /// GPU memory the renderer holds, in bytes: buffers, then render targets.
    std::array<u64, 2> memoryBytes() const;
    /// Hover probes that have come back, and those whose readback failed.
    std::array<u64, 2> probeCounts() const;
    /// Pixels of the newest probe window with an id, and with a depth, for diagnosing readbacks.
    std::array<u64, 2> probeContents() const;

private:
    Renderer();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace geo::webgpu
