// The renderer spike: cull, a visibility pass and a composite, timed on the GPU. The same code runs
// natively on Dawn and in the browser through Emscripten's WebGPU.
#pragma once

#include "scene.h"

#include <webgpu/webgpu_cpp.h>

#include <deque>
#include <initializer_list>
#include <string>

namespace geo::spike {

struct View {
    float azimuth = 35.f;   // degrees around z
    float elevation = 25.f; // degrees above the xy plane
    float zoom = 1.f;       // 1 fits the model
    i32 pickX = -1, pickY = -1; // a pixel to read the ids of, as hover does, or none
};

/// What the pixel under the cursor showed, and how long the answer took to come back.
struct PickResult {
    u32 id = 0, body = 0;
    u32 frames = 0; // frames submitted after the one it was read in
    double ms = 0;  // from submitting that frame to having the answer
    u32 tag = 0;
};

/// GPU milliseconds of one frame's passes, and how much it drew. Apple GPUs overlap consecutive
/// passes, so the parts are approximate and only the total is exact.
struct FrameTiming {
    double cull = 0, shadow = 0, vis = 0, occlusion = 0, composite = 0, total = 0;
    u64 clusters = 0;
    u32 tag = 0; // the caller's tag at the time the frame was submitted
};

/// How the visibility pass draws. Pulled: 16-triangle clusters, three vertex invocations per
/// triangle, and the composite finds each pixel's triangle again. Indexed: the cull pass writes
/// visible meshlets' indices, the hardware reuses vertices, and the pass writes the pixel's normal
/// and ids for the composite to read directly.
enum class Mode { Pulled, Indexed };

/// What the indexed mode draws besides surfaces, as the Mac app's Illustration style does. Each
/// can be left out, to measure what it costs.
struct Features {
    bool edges = true;
    bool shadow = true;
    bool occlusion = true;
};

class Renderer {
public:
    /// `callbackMode` is how timing readbacks complete: AllowProcessEvents natively, where the
    /// caller processes events, and AllowSpontaneous in the browser. `pixelsPerPoint` scales line
    /// widths, as a display's backing scale does.
    bool init(wgpu::Device device, const SceneData& scene, Mode mode, Features features,
              float pixelsPerPoint, wgpu::TextureFormat outputFormat,
              wgpu::CallbackMode callbackMode, std::string& error);
    void resize(u32 width, u32 height);
    /// Encodes and submits one frame into `target`, which must be `width` by `height`.
    void frame(const View& view, wgpu::TextureView target);
    /// Timings of finished frames, oldest first.
    std::deque<FrameTiming>& timings() { return timings_; }
    /// Answers to picks, oldest first.
    std::deque<PickResult>& picks() { return picks_; }
    bool timestamps() const { return querySet_ != nullptr; }
    /// Marks the frames submitted from now on, so late timings land in the right phase.
    void setTag(u32 tag) { frameTag_ = tag; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    /// GPU memory the renderer holds, in bytes, for buffers and for targets.
    u64 bufferBytes() const { return bufferBytes_; }
    u64 targetBytes() const { return targetBytes_; }

private:
    struct Readback {
        wgpu::Buffer buffer;
        bool busy = false;
    };
    wgpu::Buffer buffer(u64 size, wgpu::BufferUsage usage, const char* label);
    wgpu::Texture texture(u32 width, u32 height, wgpu::TextureFormat format, const char* label,
                          wgpu::TextureUsage extra = wgpu::TextureUsage::None);
    void createPipelines();
    void createTargets();
    void createBindGroups();
    template <typename Pipeline>
    wgpu::BindGroup group(const Pipeline& pipeline,
                          std::initializer_list<wgpu::BindGroupEntry> entries);

    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::ShaderModule module_;
    wgpu::CallbackMode callbackMode_ = wgpu::CallbackMode::AllowProcessEvents;
    wgpu::TextureFormat outputFormat_ = wgpu::TextureFormat::Undefined;
    Mode mode_ = Mode::Pulled;
    Features features_;
    float pixelsPerPoint_ = 1.f;

    wgpu::Buffer frameBuffer_, definitions_, occurrences_, instances_;
    wgpu::Buffer streams_[kStreams];
    wgpu::Buffer clusters_, args_;               // pulled
    wgpu::Buffer slots_, indices_, indexedArgs_; // indexed
    wgpu::Buffer shadowSlots_, shadowIndices_, shadowArgs_;
    wgpu::Buffer edgeInstances_, edgeSlots_, edgeArgs_;

    wgpu::ComputePipeline cull_, cullShadow_, cullEdges_;
    wgpu::RenderPipeline visClosed_, visOpen_, edges_, shadow_, occlusion_, composite_;
    wgpu::BindGroup cullGroup_, cullShadowGroup_, cullEdgesGroup_;
    // Pipelines with automatic layouts each need their own bind groups.
    wgpu::BindGroup visClosedGroup_, visOpenGroup_, edgesGroup_, shadowGroup_, occlusionGroup_;
    wgpu::BindGroup compositeGroup_;

    wgpu::Texture vis_, normal_, depth_, ink_, tag_, occlusionTarget_, shadowMask_;
    wgpu::TextureView visView_, normalView_, depthView_, inkView_, tagView_, occlusionView_;
    wgpu::TextureView shadowView_;

    wgpu::QuerySet querySet_;
    wgpu::Buffer resolve_;
    Readback readbacks_[4];
    Readback pickReadbacks_[3]; // the hover probe's ring, so a pick never waits on the last
    u64 frameCount_ = 0;
    std::deque<PickResult> picks_;

    u32 width_ = 0, height_ = 0;
    u32 instanceCount_ = 0, edgeInstanceCount_ = 0;
    u32 openStart_ = 0;      // where open surfaces' clusters or slots begin
    u32 openIndexStart_ = 0; // and their indices, in the indexed mode
    std::array<float, 3> center_{}, boundsMin_{};
    float radius_ = 1.f;
    u64 bufferBytes_ = 0, targetBytes_ = 0;
    std::deque<FrameTiming> timings_;
    u32 frameTag_ = 0;
};

} // namespace geo::spike
