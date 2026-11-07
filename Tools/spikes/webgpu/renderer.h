// The renderer spike: cull, a visibility pass and a composite, timed on the GPU. The same code runs
// natively on Dawn and in the browser through Emscripten's WebGPU.
#pragma once

#include "scene.h"

#include <webgpu/webgpu_cpp.h>

#include <deque>
#include <string>

namespace geo::spike {

struct View {
    float azimuth = 35.f;   // degrees around z
    float elevation = 25.f; // degrees above the xy plane
    float zoom = 1.f;       // 1 fits the model
};

/// GPU milliseconds of one frame's passes, and how many triangles it drew.
struct FrameTiming {
    double cull = 0, vis = 0, composite = 0, total = 0;
    u64 clusters = 0;
    u32 tag = 0; // the caller's tag at the time the frame was submitted
};

class Renderer {
public:
    /// `callbackMode` is how timing readbacks complete: AllowProcessEvents natively, where the
    /// caller processes events, and AllowSpontaneous in the browser.
    bool init(wgpu::Device device, const SceneData& scene, wgpu::TextureFormat outputFormat,
              wgpu::CallbackMode callbackMode, std::string& error);
    void resize(u32 width, u32 height);
    /// Encodes and submits one frame into `target`, which must be `width` by `height`.
    void frame(const View& view, wgpu::TextureView target);
    /// Timings of finished frames, oldest first.
    std::deque<FrameTiming>& timings() { return timings_; }
    bool timestamps() const { return querySet_ != nullptr; }
    /// Marks the frames submitted from now on, so late timings land in the right phase.
    void setTag(u32 tag) { tag_ = tag; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

private:
    struct Readback {
        wgpu::Buffer buffer;
        bool busy = false;
    };
    void createTargets();
    void createBindGroups();

    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::CallbackMode callbackMode_ = wgpu::CallbackMode::AllowProcessEvents;
    wgpu::TextureFormat outputFormat_ = wgpu::TextureFormat::Undefined;

    wgpu::Buffer frameBuffer_, definitions_, occurrences_, instances_, clusters_, args_;
    wgpu::Buffer streams_[kStreams];
    wgpu::ComputePipeline cull_;
    wgpu::RenderPipeline visClosed_, visOpen_, composite_;
    wgpu::BindGroup cullGroup_, visClosedGroup_, visOpenGroup_, compositeGroup_;
    wgpu::Texture vis_, depth_;
    wgpu::TextureView visView_, depthView_;
    wgpu::QuerySet querySet_;
    wgpu::Buffer resolve_;
    Readback readbacks_[4];

    u32 width_ = 0, height_ = 0;
    u32 instanceCount_ = 0;
    u32 openClusterStart_ = 0;
    std::array<float, 3> center_{};
    float radius_ = 1.f;
    std::deque<FrameTiming> timings_;
    u32 tag_ = 0;
};

} // namespace geo::spike
