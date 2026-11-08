// The engine in the browser: the core, the WebGPU renderer and a host made of the page's frame
// loop, timers and callbacks. Everything runs on the page's main thread. The page talks to it
// through the geo_web_* functions below, and it calls the page back through `Module.geoHost`:
//
//   geoHost.ready(format) / geoHost.failed(message)   once the GPU is set up, or cannot be
//   geoHost.runFrames(running)                         start or stop calling geo_web_frame
//   geoHost.event(kind, document, node, done, total, message)   a GeoEvent
//   geoHost.startImport(token, path) / cancelImport(token)      stream a file in, or stop
//   geoHost.startRefinement(token, path, level, definitions) / cancelRefinement()
//                                                      import definitions again, finer, or stop
//
// A file arrives as a cache file's bytes, streamed through geo_web_stream_*: its segments go
// straight to GPU buffers as they arrive, and memory keeps only what the CPU reads.
#include "core/core.h"
#include "scene/segment.h"
#include "webgpu/renderer.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geo;

namespace {

// How long the view must be still before its four samples are drawn, one a frame, so a touch that
// lands meanwhile waits for one sample's frame at most.
constexpr double kSettleSeconds = 0.35;

// JavaScript, which clang-format would break.
// clang-format off
EM_JS(void, jsReady, (u32 format), { Module.geoHost.ready(format); });
EM_JS(void, jsFailed, (const char* message), { Module.geoHost.failed(UTF8ToString(message)); });
EM_JS(void, jsRunFrames, (int running), { Module.geoHost.runFrames(running != 0); });
EM_JS(void, jsEvent, (u32 kind, u32 document, u32 node, u32 done, u32 total, const char* message),
      { Module.geoHost.event(kind, document, node, done, total, UTF8ToString(message)); });
EM_JS(void, jsStartImport, (double token, const char* path),
      { Module.geoHost.startImport(token, UTF8ToString(path)); });
EM_JS(void, jsCancelImport, (double token), { Module.geoHost.cancelImport(token); });
EM_JS(void, jsStartRefinement,
      (double token, const char* path, u32 level, const u32* definitions, u32 count), {
          Module.geoHost.startRefinement(token, UTF8ToString(path), level,
                                         HEAPU32.slice(definitions >> 2, (definitions >> 2) + count));
      });
EM_JS(void, jsCancelRefinement, (), { Module.geoHost.cancelRefinement(); });
EM_JS(void, jsSnapshot, (const u8* pixels, u32 width, u32 height, int bgra),
      { Module.geoHost.snapshot(pixels, width, height, bgra != 0); });
// clang-format on

class WebHost;

struct Web {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    std::string canvas;
    std::unique_ptr<webgpu::Renderer> renderer;
    std::unique_ptr<engine::Host> host;
    std::unique_ptr<engine::Core> core;
    u32 width = 1, height = 1;
    float scale = 1.0f;
    bool reduceMotion = false;
    bool phone = false;

    // The frame clock: the core's wish, and whether the loop is running.
    bool coreWantsFrames = false;
    bool looping = false;
    // The latest frame, kept to draw again with all its samples once the view is still.
    std::optional<engine::Frame> last;
    double lastDrawn = 0.0;
    bool settled = true;
    u32 settleStep = 0; // of the still view's four samples
    bool pointerMoved = false;
    u64 framesDrawn = 0, settledFrames = 0;

    int refineTimer = 0;
    struct Stream {
        std::unique_ptr<scene::CacheStream> reader;
    };
    std::unordered_map<u64, Stream> streams;
    // The refinement under way, one at a time: its segments join the document's.
    struct Refinement {
        u64 token = 0;
        u32 level = 0;
        u32 definitions = 0;
        double started = 0.0;
        std::unique_ptr<scene::CacheStream> reader;
    };
    std::optional<Refinement> refinement;

    // What the page reads back after the events that announce it.
    std::vector<std::shared_ptr<scene::Segment>> models;
    std::vector<u32> swatches, swatchBases;
    std::vector<std::vector<model::NodeVisibility>> nodeStates;
    engine::Summary summary;
    std::vector<u8> staging;
    std::string lastError; // the first GPU errors, for diagnosing
    u32 errors = 0;
};

Web web;

double nowSeconds() {
    return emscripten_get_now() / 1000.0;
}

void startLoop();

class WebHost final : public engine::Host {
public:
    double now() const override { return nowSeconds(); }
    bool reduceMotion() const override { return web.reduceMotion; }

    void runFrames(bool running) override {
        web.coreWantsFrames = running;
        if (running) {
            startLoop();
        }
    }

    void scheduleRefinement(double delaySeconds) override {
        if (web.refineTimer != 0) {
            emscripten_clear_timeout(web.refineTimer);
        }
        web.refineTimer = emscripten_set_timeout(
            [](void*) {
                web.refineTimer = 0;
                web.core->refinementDue();
            },
            delaySeconds * 1000.0, nullptr);
    }

    void startImport(u64 token, const std::string& path) override {
        web.streams[token].reader = std::make_unique<scene::CacheStream>(
            web.renderer.get(), [token](std::shared_ptr<scene::Segment> segment) {
                web.core->addSegment(token, segment);
            });
        jsStartImport(double(token), path.c_str());
    }

    // The page imports the definitions again, finer, with OCCT in its workers, and hands their
    // segments over through geo_web_refine_segment, ending with geo_web_refine_end.
    void startRefinement(u64 token, const std::string& path, u32 level,
                         std::vector<u32> definitions) override {
        web.refinement.emplace();
        Web::Refinement& r = *web.refinement;
        r.token = token;
        r.level = level;
        r.definitions = u32(definitions.size());
        r.started = nowSeconds();
        r.reader = std::make_unique<scene::CacheStream>(
            web.renderer.get(), [token](std::shared_ptr<scene::Segment> segment) {
                web.core->addSegment(token, segment);
            });
        jsStartRefinement(double(token), path.c_str(), level, definitions.data(),
                          u32(definitions.size()));
    }

    void cancelImport(u64 token) override {
        if (web.streams.erase(token) != 0) {
            jsCancelImport(double(token));
        }
    }
    void cancelRefinement() override {
        if (web.refinement) {
            web.refinement.reset();
            jsCancelRefinement();
        }
    }
    void evictCache(std::vector<std::string>) override {}

    void emit(const GeoEvent& event) override {
        jsEvent(u32(event.kind), event.document, event.node, event.done, event.total,
                event.message != nullptr ? event.message : "");
    }

    void publishModel(u32 document, std::shared_ptr<scene::Segment> model) override {
        web.models.resize(std::max<usize>(web.models.size(), document + 1));
        web.models[document] = std::move(model);
    }
    void clearModels() override { web.models.clear(); }
    void publishSwatches(std::vector<u32> swatches, std::vector<u32> bases) override {
        web.swatches = std::move(swatches);
        web.swatchBases = std::move(bases);
    }
    void publishNodeStates(std::vector<std::vector<model::NodeVisibility>> states) override {
        web.nodeStates = std::move(states);
    }
    void publishSummary(const engine::Summary& summary) override { web.summary = summary; }
};

void configureSurface() {
    wgpu::SurfaceConfiguration configuration{};
    configuration.device = web.device;
    configuration.format = web.format;
    configuration.width = web.width;
    configuration.height = web.height;
    configuration.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    web.surface.Configure(&configuration);
}

// Draws a frame with one sample, or a still view's step of its four.
void draw(const engine::Frame& frame, std::optional<u32> step = std::nullopt) {
    wgpu::SurfaceTexture surfaceTexture;
    web.surface.GetCurrentTexture(&surfaceTexture);
    if (!surfaceTexture.texture) {
        return;
    }
    if (step) {
        web.renderer->renderStep(surfaceTexture.texture, frame.view, &frame.overlay, *step);
    } else {
        web.renderer->render(surfaceTexture.texture, frame.view, &frame.overlay);
    }
}

// One display refresh. The core decides whether anything changed; once the view has been still a
// moment, its four samples are drawn, one a frame, and then nothing is submitted until something
// changes again.
bool tick(double, void*) {
    if (web.pointerMoved) {
        web.pointerMoved = false;
        web.core->pointerSettled();
    }
    const double now = nowSeconds();
    if (std::optional<engine::Frame> frame = web.core->frame()) {
        web.renderer->setSamples(1);
        draw(*frame);
        web.last = std::move(frame);
        web.lastDrawn = now;
        web.settled = false;
        web.settleStep = 0;
        ++web.framesDrawn;
    } else if (!web.settled && web.last && now - web.lastDrawn >= kSettleSeconds) {
        draw(*web.last, web.settleStep);
        if (++web.settleStep == 4) {
            web.settled = true;
            ++web.settledFrames;
        }
    }
    web.looping = web.coreWantsFrames || !web.settled;
    if (!web.looping) {
        jsRunFrames(0);
    }
    return web.looping;
}

void startLoop() {
    if (!web.looping && web.core) {
        web.looping = true;
        jsRunFrames(1);
        emscripten_request_animation_frame_loop(tick, nullptr);
    }
}

void deviceReady() {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.selector = web.canvas.c_str();
    wgpu::SurfaceDescriptor surfaceDescriptor{};
    surfaceDescriptor.nextInChain = &canvas;
    web.surface = web.instance.CreateSurface(&surfaceDescriptor);
    wgpu::SurfaceCapabilities capabilities;
    web.surface.GetCapabilities(web.adapter, &capabilities);
    web.format = capabilities.formats[0];
    configureSurface();
    std::string error;
    web.renderer = webgpu::Renderer::create(web.instance, web.device, web.format, error);
    if (!web.renderer) {
        jsFailed(error.c_str());
        return;
    }
    web.host = std::make_unique<WebHost>();
    web.core = std::make_unique<engine::Core>(*web.renderer, *web.host);
    if (web.phone) {
        // A phone draws about 3 million triangles a frame at 60 Hz: more waits until still.
        web.core->setDetailBudget(3'000'000, 400'000);
        // Refined geometry stays on the GPU beside the coarser: a phone, with far less memory,
        // goes to a quarter of the coarsest chord error, rather than a sixteenth.
        web.core->setRefinementLimit(2);
    }
    web.core->resize(web.width, web.height, web.scale);
    jsReady(u32(web.format));
}

} // namespace

extern "C" {

/// Starts the engine on the canvas `selector`, `width` by `height` pixels at `scale` pixels per
/// CSS pixel. `flags`: 1 reduce motion, 2 a phone. Answers through geoHost.ready or .failed.
EMSCRIPTEN_KEEPALIVE void geo_web_start(const char* selector, u32 width, u32 height, float scale,
                                        u32 flags) {
    web.canvas = selector;
    web.width = std::max(width, 1u);
    web.height = std::max(height, 1u);
    web.scale = scale;
    web.reduceMotion = (flags & 1) != 0;
    web.phone = (flags & 2) != 0;
    web.instance = wgpu::CreateInstance(nullptr);
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    web.instance.RequestAdapter(
        &options, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status != wgpu::RequestAdapterStatus::Success) {
                jsFailed(std::string(message.data, message.length).c_str());
                return;
            }
            web.adapter = std::move(adapter);
            wgpu::Limits adapterLimits{};
            web.adapter.GetLimits(&adapterLimits);
            static std::vector<wgpu::FeatureName> features;
            if (web.adapter.HasFeature(wgpu::FeatureName::TimestampQuery)) {
                features.push_back(wgpu::FeatureName::TimestampQuery);
            }
            static wgpu::Limits limits{};
            limits.maxBufferSize = adapterLimits.maxBufferSize;
            limits.maxStorageBufferBindingSize = adapterLimits.maxStorageBufferBindingSize;
            static wgpu::DeviceDescriptor descriptor{};
            descriptor.requiredFeatureCount = features.size();
            descriptor.requiredFeatures = features.data();
            descriptor.requiredLimits = &limits;
            descriptor.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView error) {
                    if (web.errors < 3) {
                        web.lastError += std::string(error.data, error.length) + " | ";
                    }
                    ++web.errors;
                    std::fprintf(stderr, "webgpu: %.*s\n", int(error.length), error.data);
                });
            web.adapter.RequestDevice(
                &descriptor, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus status, wgpu::Device device,
                   wgpu::StringView message) {
                    if (status != wgpu::RequestDeviceStatus::Success) {
                        jsFailed(std::string(message.data, message.length).c_str());
                        return;
                    }
                    web.device = std::move(device);
                    deviceReady();
                });
        });
}

EMSCRIPTEN_KEEPALIVE void geo_web_resize(u32 width, u32 height, float scale) {
    web.width = std::max(width, 1u);
    web.height = std::max(height, 1u);
    web.scale = scale;
    if (web.core) {
        configureSurface();
        web.core->resize(web.width, web.height, scale);
        web.core->requestFrame();
    }
}

EMSCRIPTEN_KEEPALIVE void geo_web_set_insets(float left, float top, float right, float bottom) {
    web.core->setInsets({left, top, right, bottom});
}

EMSCRIPTEN_KEEPALIVE void geo_web_pointer(u32 kind, u32 button, float x, float y, float delta,
                                          u32 modifiers, double timestamp) {
    web.core->pointer(
        {GeoPointerKind(kind), GeoPointerButton(button), x, y, delta, modifiers, timestamp});
    web.pointerMoved = true;
    startLoop();
}

EMSCRIPTEN_KEEPALIVE void geo_web_perform(u32 action) {
    web.core->perform(GeoAction(action));
}
EMSCRIPTEN_KEEPALIVE void geo_web_set_selection_mode(u32 mode) {
    web.core->setSelectionMode(GeoSelectionMode(mode));
}
EMSCRIPTEN_KEEPALIVE void geo_web_set_tool(u32 tool) {
    web.core->setTool(GeoTool(tool));
}

EMSCRIPTEN_KEEPALIVE void geo_web_set_look(u32 style, u32 colorMode, u32 ghostStyle, int castShadow,
                                           float grain, float wobble) {
    web.core->setLook({GeoStyle(style), GeoColorMode(colorMode), GeoGhostStyle(ghostStyle),
                       castShadow != 0, grain, wobble});
}

/// Bits 0-2 enable cuts across x, y and z; bits 4-6 flip them.
EMSCRIPTEN_KEEPALIVE void geo_web_set_section(u32 bits, float x, float y, float z) {
    GeoSection section{};
    const float position[3] = {x, y, z};
    for (int axis = 0; axis < 3; ++axis) {
        section.enabled[axis] = (bits & (1u << axis)) != 0;
        section.flip[axis] = (bits & (16u << axis)) != 0;
        section.position[axis] = position[axis];
    }
    web.core->setSection(section);
}

EMSCRIPTEN_KEEPALIVE void geo_web_select_node(u32 document, u32 node, int extend) {
    web.core->selectNode(document, node, extend != 0);
}
EMSCRIPTEN_KEEPALIVE void geo_web_hover_node(u32 document, u32 node) {
    web.core->hoverNode(document, node);
}
EMSCRIPTEN_KEEPALIVE void geo_web_node_action(u32 document, u32 node, u32 action) {
    web.core->nodeAction(document, node, GeoNodeAction(action));
}

/// Opens a file the page knows by `path`, replacing what is open or added to it. The core
/// answers with geoHost.startImport.
EMSCRIPTEN_KEEPALIVE void geo_web_open(const char* path, int add) {
    web.core->openFile(path, add != 0);
}

/// Where the page copies a file's bytes before handing them over: at least `size` bytes, reused.
EMSCRIPTEN_KEEPALIVE u8* geo_web_staging(u32 size) {
    if (web.staging.size() < size) {
        web.staging.resize(size);
    }
    return web.staging.data();
}

/// The size of the file about to be streamed for `token`, where the page knows it.
EMSCRIPTEN_KEEPALIVE void geo_web_stream_size(double token, double size) {
    if (const auto found = web.streams.find(u64(token)); found != web.streams.end()) {
        found->second.reader->expectSize(u64(size));
    }
}

/// The next `size` bytes of the file being imported for `token`, at `bytes`. Returns 1 to go on,
/// 0 if the import was cancelled, -1 if the file proved invalid, which ends the import.
EMSCRIPTEN_KEEPALIVE int geo_web_stream_chunk(double token, const u8* bytes, u32 size) {
    const auto found = web.streams.find(u64(token));
    if (found == web.streams.end()) {
        return 0;
    }
    Web::Stream& stream = found->second;
    std::string error;
    if (!stream.reader->append({bytes, size}, error)) {
        web.streams.erase(found);
        web.core->importFinished(u64(token), {false, true, error, ""});
        return -1;
    }
    if (stream.reader->size() != 0) {
        web.core->importProgress(u64(token), u32(stream.reader->received() >> 10),
                                 u32(stream.reader->size() >> 10));
    }
    return 1;
}

/// One whole segment of a file being imported for `token`, as the importer wrote it, so the model
/// shows while it imports. Returns 1 to go on, 0 if the import was cancelled, -1 if the segment
/// proved invalid, which ends the import.
EMSCRIPTEN_KEEPALIVE int geo_web_import_segment(double token, const u8* bytes, u32 size) {
    const auto found = web.streams.find(u64(token));
    if (found == web.streams.end()) {
        return 0;
    }
    std::string error;
    if (!found->second.reader->takeSegment({bytes, size}, error)) {
        web.streams.erase(found);
        web.core->importFinished(u64(token), {false, false, error, ""});
        return -1;
    }
    return 1;
}

/// The import of `token` has finished, every segment taken, or failed, as `message` says.
EMSCRIPTEN_KEEPALIVE void geo_web_import_end(double token, const char* message) {
    const auto found = web.streams.find(u64(token));
    if (found == web.streams.end()) {
        return;
    }
    const std::string error = message != nullptr ? message : "";
    web.streams.erase(found);
    web.core->importFinished(u64(token), {error.empty(), false, error, ""});
}

/// One whole segment of the refinement of `token`'s document. Returns 1 to go on, 0 if the
/// refinement was cancelled, -1 if the segment proved invalid, which ends the refinement.
EMSCRIPTEN_KEEPALIVE int geo_web_refine_segment(double token, const u8* bytes, u32 size) {
    if (!web.refinement || web.refinement->token != u64(token)) {
        return 0;
    }
    std::string error;
    if (!web.refinement->reader->takeSegment({bytes, size}, error)) {
        const Web::Refinement r = std::move(*web.refinement);
        web.refinement.reset();
        web.core->refinementFinished(r.token, false, r.definitions, r.level, r.started);
        return -1;
    }
    return 1;
}

/// The refinement of `token`'s document has ended: every segment handed over if `ok`.
EMSCRIPTEN_KEEPALIVE void geo_web_refine_end(double token, int ok) {
    if (!web.refinement || web.refinement->token != u64(token)) {
        return;
    }
    const Web::Refinement r = std::move(*web.refinement);
    web.refinement.reset();
    web.core->refinementFinished(r.token, ok != 0, r.definitions, r.level, r.started);
}

/// The file has ended, or could not be read, as `message` says.
EMSCRIPTEN_KEEPALIVE void geo_web_stream_end(double token, const char* message) {
    const auto found = web.streams.find(u64(token));
    if (found == web.streams.end()) {
        return;
    }
    std::string error = message != nullptr ? message : "";
    const bool ok = error.empty() && found->second.reader->finish(error);
    web.streams.erase(found);
    web.core->importFinished(u64(token), {ok, true, error, ""});
}

/// Draws the current view with all its samples into a texture and reads it back; the pixels
/// arrive through geoHost.snapshot, valid only during that call.
EMSCRIPTEN_KEEPALIVE void geo_web_snapshot() {
    // The latest view, made here rather than waiting for the frame loop, which a browser pauses
    // while the page is out of sight, as it is when an AI app asks for a picture.
    if (std::optional<engine::Frame> frame = web.core->frame()) {
        web.last = std::move(frame);
    }
    if (!web.last) {
        jsSnapshot(nullptr, 0, 0, 0);
        return;
    }
    wgpu::TextureDescriptor descriptor{};
    descriptor.size = {web.width, web.height, 1};
    descriptor.format = web.format;
    descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    const wgpu::Texture target = web.device.CreateTexture(&descriptor);
    web.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    web.renderer->setSamples(4);
    web.renderer->render(target, web.last->view, &web.last->overlay);
    web.renderer->setSamples(1);
    web.device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous,
                             [](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                                wgpu::StringView message) {
                                 if (type != wgpu::ErrorType::NoError && web.errors++ < 3) {
                                     web.lastError +=
                                         "snapshot: " + std::string(message.data, message.length);
                                 }
                             });
    const u32 stride = (web.width * 4 + 255) & ~255u;
    wgpu::BufferDescriptor bufferDescriptor{};
    bufferDescriptor.size = u64(stride) * web.height;
    bufferDescriptor.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    const wgpu::Buffer readback = web.device.CreateBuffer(&bufferDescriptor);
    wgpu::TexelCopyTextureInfo source{};
    source.texture = target;
    wgpu::TexelCopyBufferInfo destination{};
    destination.buffer = readback;
    destination.layout.bytesPerRow = stride;
    const wgpu::Extent3D extent = {web.width, web.height, 1};
    const wgpu::CommandEncoder encoder = web.device.CreateCommandEncoder();
    encoder.CopyTextureToBuffer(&source, &destination, &extent);
    const wgpu::CommandBuffer commands = encoder.Finish();
    web.device.GetQueue().Submit(1, &commands);
    const u32 width = web.width, height = web.height;
    readback.MapAsync(
        wgpu::MapMode::Read, 0, usize(bufferDescriptor.size), wgpu::CallbackMode::AllowSpontaneous,
        [readback, stride, width, height](wgpu::MapAsyncStatus status, wgpu::StringView) {
            if (status != wgpu::MapAsyncStatus::Success) {
                jsSnapshot(nullptr, 0, 0, 0);
                return;
            }
            const auto* bytes =
                static_cast<const u8*>(readback.GetConstMappedRange(0, usize(stride) * height));
            std::vector<u8> pixels(usize(width) * height * 4);
            for (u32 y = 0; y < height; ++y) {
                std::copy_n(bytes + usize(y) * stride, width * 4,
                            pixels.data() + usize(y) * width * 4);
            }
            readback.Unmap();
            jsSnapshot(pixels.data(), width, height,
                       web.format == wgpu::TextureFormat::BGRA8Unorm ? 1 : 0);
        });
}

/// The overlay's text, as the page drew it: kAtlasWidth by kAtlasHeight bytes of coverage.
EMSCRIPTEN_KEEPALIVE void geo_web_set_atlas(const u8* pixels, u32 size, float glyphAdvance) {
    web.renderer->setOverlayAtlas({pixels, size}, glyphAdvance);
    web.core->requestFrame();
}

EMSCRIPTEN_KEEPALIVE const char* geo_web_last_error() { return web.lastError.c_str(); }

EMSCRIPTEN_KEEPALIVE void geo_web_request_frame() {
    web.core->requestFrame();
}

/// Looks at what is shown from a direction, framed: for pictures an AI app asks for.
EMSCRIPTEN_KEEPALIVE void geo_web_set_view(double azimuthDegrees, double elevationDegrees,
                                           int perspective) {
    web.core->setViewDirection(azimuthDegrees, elevationDegrees, perspective != 0);
}

/// The scene's bounds (min x, y, z, then max), its face count and its triangle count; zeros while
/// nothing is open.
EMSCRIPTEN_KEEPALIVE void geo_web_scene_info(double* out) {
    const std::optional<Aabb> bounds = web.core->sceneBounds();
    const double values[8] = {bounds ? bounds->min.x : 0, bounds ? bounds->min.y : 0,
                              bounds ? bounds->min.z : 0, bounds ? bounds->max.x : 0,
                              bounds ? bounds->max.y : 0, bounds ? bounds->max.z : 0,
                              double(web.core->sceneFaces()), double(web.core->sceneTriangles())};
    std::copy(std::begin(values), std::end(values), out);
}

// ---- What the page reads back ----------------------------------------------------------------

EMSCRIPTEN_KEEPALIVE u32 geo_web_model_node_count(u32 document) {
    return document < web.models.size() && web.models[document]
               ? u32(web.models[document]->view().nodes.size())
               : 0;
}
/// Rows of eight words, as GeoNode lays them out.
EMSCRIPTEN_KEEPALIVE const format::NodeRecord* geo_web_model_nodes(u32 document) {
    return geo_web_model_node_count(document) != 0 ? web.models[document]->view().nodes.data()
                                                   : nullptr;
}
EMSCRIPTEN_KEEPALIVE const char* geo_web_model_strings(u32 document) {
    return geo_web_model_node_count(document) != 0 ? web.models[document]->view().strings.data()
                                                   : nullptr;
}
EMSCRIPTEN_KEEPALIVE u32 geo_web_model_strings_size(u32 document) {
    return geo_web_model_node_count(document) != 0
               ? u32(web.models[document]->view().strings.size())
               : 0;
}
/// One NodeVisibility byte per tree row.
EMSCRIPTEN_KEEPALIVE const model::NodeVisibility* geo_web_node_states(u32 document) {
    return document < web.nodeStates.size() ? web.nodeStates[document].data() : nullptr;
}
EMSCRIPTEN_KEEPALIVE u32 geo_web_node_state_count(u32 document) {
    return document < web.nodeStates.size() ? u32(web.nodeStates[document].size()) : 0;
}
EMSCRIPTEN_KEEPALIVE const u32* geo_web_swatches() {
    return web.swatches.data();
}
EMSCRIPTEN_KEEPALIVE u32 geo_web_swatch_count() {
    return u32(web.swatches.size());
}
EMSCRIPTEN_KEEPALIVE const u32* geo_web_swatch_bases() {
    return web.swatchBases.data();
}
EMSCRIPTEN_KEEPALIVE u32 geo_web_swatch_base_count() {
    return u32(web.swatchBases.size());
}

/// The selection card's numbers: occurrences, faces, document, node, mode, then the selected and
/// hidden occurrence counts.
EMSCRIPTEN_KEEPALIVE void geo_web_summary(u32* out) {
    const GeoSelectionSummary& s = web.summary.selection;
    const u32 values[7] = {s.occurrences,
                           s.faces,
                           s.document,
                           s.node,
                           s.mode,
                           web.summary.selectedOccurrences,
                           web.summary.hiddenOccurrences};
    std::copy(std::begin(values), std::end(values), out);
}

/// For tests and measurements: frames drawn moving and settled, GPU milliseconds of the latest
/// timed frame, GPU buffer and target megabytes, triangles and edge segments in the scene,
/// whether the frame loop is running, and the hovered pick id.
EMSCRIPTEN_KEEPALIVE void geo_web_stats(double* out) {
    const std::array<u64, 2> memory = web.renderer->memoryBytes();
    out[0] = double(web.framesDrawn);
    out[1] = double(web.settledFrames);
    out[2] = web.renderer->gpuMilliseconds();
    out[3] = double(memory[0]) / 1048576.0;
    out[4] = double(memory[1]) / 1048576.0;
    out[5] = double(web.renderer->sceneTriangles());
    out[6] = double(web.renderer->sceneEdgeSegments());
    out[7] = web.looping ? 1.0 : 0.0;
    out[8] = double(web.core->hoveredId());
}

} // extern "C"
