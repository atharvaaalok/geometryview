// The WebGPU renderer: the Metal renderer's frames, drawn from WGSL ports of its shaders, for the
// universal viewer. It runs natively on Dawn, which tests it against Metal, and in the browser.
//
// Where Metal has mesh shaders, a compute pass culls meshlets and writes the visible ones' indices,
// and one indexed draw per list pulls their vertices from the cache's own records. Where Metal has
// four samples in tile memory, the main pass has one, and antialiasing comes from jittered passes
// at the same four sample positions, accumulated. See ADR 0014.
#pragma once

#include "render/backend.h"

#include <webgpu/webgpu_cpp.h>

#include <memory>
#include <string>

namespace geo::webgpu {

class Renderer final : public render::Backend {
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
    std::vector<u8> renderOffscreen(const render::ViewState& view,
                                    const render::Overlay* overlay = nullptr) override;

    /// Draws a frame into `target`, which must be the size last given to `resize`.
    void render(const wgpu::Texture& target, const render::ViewState& view,
                const render::Overlay* overlay = nullptr);

    /// Jittered samples per frame, from 1 to 4. Offscreen renders always take 4.
    void setSamples(u32 samples);

    /// The GPU time of the latest frame timed, from timestamps, where the device has them, and
    /// how many frames have been timed so far.
    double gpuMilliseconds() const;
    u64 timedFrames() const;

private:
    Renderer();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace geo::webgpu
