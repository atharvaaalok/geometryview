// Implements the C API and runs the engine thread.
//
// The main thread only ever enqueues. The engine thread drains the queue, updates view state and
// renders, which makes it the single writer of that state and removes most locking.
#include "engine.h"

#include "base/assert.h"
#include "base/spsc_ring.h"
#include "base/time.h"
#include "base/trace.h"
#include "engine/allocation_counter.h"
#include "engine/latency_probe.h"
#include "gpu/frame_source.h"
#include "gpu/renderer.h"
#include "ipc/import_job.h"
#include "nav/camera.h"
#include "nav/orbit_policy.h"
#include "nav/pointer_controller.h"
#include "render/view.h"
#include "scene/scene.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <pthread.h>
#include <string>
#include <sys/proc_info.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using namespace geo;

// ProMotion drops its refresh rate when presents stop, so frames keep flowing briefly after the
// last input, which keeps the next interaction at full rate.
constexpr double kLingerSeconds = 0.25;
constexpr usize kCommandCapacity = 4096;
constexpr double kFitMargin = 1.12;
constexpr double kDefaultAzimuth = 35.0 * M_PI / 180.0;
constexpr double kDefaultElevation = 25.0 * M_PI / 180.0;

// Seconds since this process started, for measuring launch.
double secondsSinceProcessStart() {
    proc_bsdinfo info{};
    if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        return -1.0;
    }
    timeval now{};
    gettimeofday(&now, nullptr);
    return double(now.tv_sec - i64(info.pbi_start_tvsec)) +
           double(now.tv_usec - i32(info.pbi_start_tvusec)) * 1e-6;
}

struct ResizeCommand {
    u32 width, height;
    float scale;
};

struct Command {
    enum class Kind : u8 { AttachLayer, Resize, Pointer, OpenFile, Action, RequestFrame, Stop };
    Kind kind;
    union {
        void* layer;
        ResizeCommand resize;
        GeoPointerEvent pointer;
        char* path; // owned, released by the engine thread
        GeoAction action;
    };
};

nav::Button toButton(GeoPointerButton button) {
    switch (button) {
    case GeoButtonLeft:
        return nav::Button::Left;
    case GeoButtonRight:
        return nav::Button::Right;
    case GeoButtonMiddle:
        return nav::Button::Middle;
    case GeoButtonNone:
        return nav::Button::None;
    }
    return nav::Button::None;
}

} // namespace

struct GeoEngine {
    SpscRing<Command, kCommandCapacity> commands;
    CFRunLoopRef runLoop = nullptr;
    CFRunLoopSourceRef source = nullptr;
    pthread_t thread{};
    std::atomic<bool> ready{false};

    id<MTLDevice> device = nil;
    std::unique_ptr<gpu::Renderer> renderer;
    gpu::DisplayLinkFrameSource frames;

    ipc::ImportPaths importPaths;
    GeoEventCallback eventCallback = nullptr;
    void* eventContext = nullptr;
    u64 openGeneration = 0; // a newer open supersedes one still importing

    scene::Scene scene;
    Aabb bounds{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
    bool sceneChanged = false;
    bool hasFramed = false;
    bool userMovedCamera = false;
    std::shared_ptr<ipc::ImportCancel> activeImport;

    nav::Camera camera;
    nav::Viewport viewport;
    nav::OrbitPolicy orbit;
    nav::PointerController pointer;
    render::DepthDecoder depthDecoder;

    float cursorX = -1, cursorY = -1;
    bool cursorInside = false;
    u32 hoveredId = 0;
    u32 selectedId = 0;

    bool needsFrame = false;
    bool stopping = false;
    double lastActivity = 0.0;
    double pendingInputTime = 0.0;
    const bool traceLaunch = traceEnabled();
    engine::LatencyProbe latency;

    std::atomic<u64> framesRendered{0};
    std::atomic<double> lastFrameSeconds{0.0};
    std::atomic<u64> frameAllocations{0};
    std::atomic<u32> publishedHover{0};
    std::atomic<u32> publishedSelection{0};

    GeoEngine()
        : pointer(camera, viewport, orbit, [this](double x, double y) { return pickAt(x, y); }) {}

    void post(const Command& command) {
        if (commands.tryPush(command)) {
            CFRunLoopSourceSignal(source);
            CFRunLoopWakeUp(runLoop);
        }
    }

    void emit(GeoEventKind kind, const char* message) {
        if (traceLaunch) {
            std::fprintf(stderr, "trace: event %d: %s\n", int(kind), message);
        }
        if (eventCallback != nullptr) {
            eventCallback(eventContext, kind, message);
        }
    }

    void requestFrame() {
        needsFrame = true;
        lastActivity = nowSeconds();
        frames.setRunning(true);
    }

    std::optional<nav::SurfaceHit> pickAt(double x, double y) {
        const std::optional<gpu::PixelHit> pixel =
            renderer->readPixel(int(x), int(y), depthDecoder);
        if (!pixel) {
            return std::nullopt;
        }
        const double ndcX = 2.0 * x / viewport.width - 1.0;
        const double ndcY = 1.0 - 2.0 * y / viewport.height;
        return nav::SurfaceHit{pixel->id, camera.unproject(ndcX, ndcY, pixel->viewDepth, viewport),
                               pixel->viewDepth};
    }

    // Reading the ID buffer costs nanoseconds and no GPU work. A frame is drawn only if the surface
    // under the pointer actually changed.
    void updateHover() {
        if (pointer.isPressed()) {
            return;
        }
        u32 id = 0;
        if (cursorInside) {
            if (const std::optional<nav::SurfaceHit> hit = pickAt(cursorX, cursorY)) {
                id = hit->id;
            }
        }
        if (id != hoveredId) {
            hoveredId = id;
            requestFrame();
        }
    }

    void handlePointer(const GeoPointerEvent& event) {
        lastActivity = nowSeconds();
        cursorX = event.x;
        cursorY = event.y;
        cursorInside = true;

        nav::PointerResponse response;
        switch (event.kind) {
        case GeoPointerMove:
            response = pointer.move(event.x, event.y);
            break;
        case GeoPointerDown:
            frames.setRunning(true);
            response = pointer.press(toButton(event.button), event.modifiers, event.x, event.y);
            break;
        case GeoPointerUp:
            response = pointer.release(toButton(event.button), event.x, event.y);
            break;
        case GeoPointerScroll:
        case GeoPointerMagnify:
            response = pointer.scroll(event.delta, event.x, event.y, event.timestamp);
            break;
        }

        if (response.cameraMoved) {
            userMovedCamera = true;
            pendingInputTime = pendingInputTime == 0.0 ? event.timestamp : pendingInputTime;
            requestFrame();
        }
        if (response.clicked) {
            selectedId = response.clickedId.value_or(0);
            requestFrame();
        }
    }

    void resetCamera() {
        const Vec3 direction = simd_make_double3(
            std::cos(kDefaultElevation) * std::sin(kDefaultAzimuth),
            -std::cos(kDefaultElevation) * std::cos(kDefaultAzimuth), std::sin(kDefaultElevation));
        camera.lookAt(bounds.center() + direction, bounds.center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, bounds, viewport, kFitMargin);
        orbit.setVisibleBounds(bounds);
    }

    void perform(GeoAction action) {
        lastActivity = nowSeconds();
        switch (action) {
        case GeoActionFit:
            if (scene.hasGeometry()) {
                nav::fit(camera, bounds, viewport, kFitMargin);
                orbit.navigated();
            }
            break;
        case GeoActionClearSelection:
            selectedId = 0;
            break;
        case GeoActionToggleProjection:
            camera.perspective = !camera.perspective;
            if (scene.hasGeometry()) {
                nav::fit(camera, bounds, viewport, kFitMargin);
            }
            orbit.navigated();
            break;
        }
        requestFrame();
    }

    // Delivers import results to the engine thread, tagged so a superseded import is ignored.
    class ImportBridge final : public ipc::ImportListener {
    public:
        ImportBridge(GeoEngine* engine, u64 generation)
            : engine_(engine), generation_(generation) {}

        void segmentReady(std::shared_ptr<scene::MappedSegment> segment) override {
            GeoEngine* engine = engine_;
            const u64 generation = generation_;
            engine->perform([=] { engine->addSegment(generation, segment); });
        }
        void progress(u32 done, u32 total) override {
            GeoEngine* engine = engine_;
            const u64 generation = generation_;
            engine->perform([=] { engine->reportProgress(generation, done, total); });
        }
        void warning(const std::string& text) override {
            std::fprintf(stderr, "import: %s\n", text.c_str());
        }

    private:
        GeoEngine* engine_;
        u64 generation_;
    };

    // Runs `work` on the engine thread.
    template <typename Work> void perform(Work work) {
        CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
            @autoreleasepool {
                work();
            }
        });
        CFRunLoopWakeUp(runLoop);
    }

    // By value: a block copies C++ objects it captures, but only the reference if given one, and
    // this block outlives the caller's string.
    void openFile(std::string path) {
        const u64 generation = ++openGeneration;
        const std::string name = [@(path.c_str()) lastPathComponent].UTF8String;
        emit(GeoEventLoading, name.c_str());
        const double start = nowSeconds();

        if (activeImport) {
            activeImport->cancel();
        }
        activeImport = std::make_shared<ipc::ImportCancel>();
        std::shared_ptr<ipc::ImportCancel> cancel = activeImport;

        scene.clear();
        scene.addDocument();
        sceneChanged = true;
        hasFramed = false;
        userMovedCamera = false;
        hoveredId = 0;
        selectedId = 0;
        requestFrame();

        // The import can take seconds, and the engine thread must keep drawing.
        const ipc::ImportPaths paths = importPaths;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            ImportBridge bridge(this, generation);
            ipc::ImportOutcome outcome = ipc::importFile(path, paths, bridge, cancel.get());
            perform([=, this] { finishOpen(generation, outcome, start); });
        });
    }

    void addSegment(u64 generation, const std::shared_ptr<scene::MappedSegment>& segment) {
        if (generation != openGeneration) {
            return;
        }
        std::string error;
        if (!scene.document(0).add(segment, error)) {
            emit(GeoEventFailed, error.c_str());
            return;
        }
        const bool hadBounds = hasFramed;
        sceneChanged = true;
        if (!hadBounds) {
            frameScene(true);
        }
        requestFrame();
    }

    void reportProgress(u64 generation, u32 done, u32 total) {
        if (generation == openGeneration && traceLaunch) {
            std::fprintf(stderr, "trace: imported %u of %u\n", done, total);
        }
    }

    // Takes new scene bounds, and frames them unless the user has already moved the camera.
    void frameScene(bool reset) {
        const std::optional<Aabb> current = scene.bounds();
        if (!current) {
            return;
        }
        bounds = *current;
        if (reset) {
            resetCamera();
            hasFramed = true;
        } else if (!userMovedCamera) {
            nav::fit(camera, bounds, viewport, kFitMargin);
            orbit.setVisibleBounds(bounds);
        }
    }

    void finishOpen(u64 generation, const ipc::ImportOutcome& outcome, double start) {
        if (generation != openGeneration) {
            return; // the user has already opened something else
        }
        if (!outcome.ok) {
            emit(GeoEventFailed, outcome.message.c_str());
            return;
        }
        frameScene(false); // estimated bounds become exact now that all geometry is here
        requestFrame();
        if (traceLaunch) {
            std::fprintf(stderr, "trace: scene ready %.1f ms after opening\n",
                         (nowSeconds() - start) * 1e3);
        }
        emit(GeoEventSceneReady, "");
    }

    void drainCommands() {
        bool pointerMoved = false;
        while (auto command = commands.tryPop()) {
            switch (command->kind) {
            case Command::Kind::AttachLayer:
                attachLayer((__bridge_transfer CAMetalLayer*)command->layer);
                break;
            case Command::Kind::Resize:
                viewport = {double(command->resize.width), double(command->resize.height)};
                pointer.setPixelsPerPoint(command->resize.scale);
                renderer->resize(command->resize.width, command->resize.height);
                orbit.navigated();
                requestFrame();
                break;
            case Command::Kind::Pointer:
                handlePointer(command->pointer);
                pointerMoved = true;
                break;
            case Command::Kind::OpenFile:
                openFile(command->path);
                std::free(command->path);
                break;
            case Command::Kind::Action:
                perform(command->action);
                break;
            case Command::Kind::RequestFrame:
                requestFrame();
                break;
            case Command::Kind::Stop:
                stopping = true;
                CFRunLoopStop(runLoop);
                break;
            }
        }
        if (pointerMoved) {
            updateHover();
        }
    }

    void attachLayer(CAMetalLayer* layer) {
        renderer->attachLayer(layer);
        frames.attach(layer, [this](const gpu::FrameTick& tick) { onFrame(tick); });
        requestFrame();
    }

    void onFrame(const gpu::FrameTick& tick) {
        // The surface under a still pointer changes as the camera moves, so hover is checked here
        // too.
        updateHover();
        if (needsFrame) {
            needsFrame = false;
            const double start = nowSeconds();
            if (sceneChanged) {
                sceneChanged = false;
                renderer->syncScene(scene);
            }
            render::ViewState view = render::makeView(camera, viewport, bounds);
            view.uniforms.hoveredId = hoveredId;
            view.uniforms.selectedId = selectedId;
            publishedHover.store(hoveredId, std::memory_order_relaxed);
            publishedSelection.store(selectedId, std::memory_order_relaxed);
            depthDecoder = view.depth;

            const bool firstFrame =
                traceLaunch && framesRendered.load(std::memory_order_relaxed) == 0;
            if (firstFrame) {
                [tick.drawable addPresentedHandler:^(id<MTLDrawable> drawable) {
                    (void)drawable;
                    std::fprintf(stderr,
                                 "trace: first frame presented %.1f ms after process start\n",
                                 secondsSinceProcessStart() * 1e3);
                    geo_trace_memory("first frame presented");
                }];
            }
            if (latency.enabled() && pendingInputTime != 0.0) {
                latency.track(tick.drawable, pendingInputTime, start, tick.commitDeadline,
                              tick.presentationTime);
            }
            pendingInputTime = 0.0;
            const u64 allocationsBefore = engine::allocationCount();
            renderer->render(tick.drawable, view);
            lastFrameSeconds.store(nowSeconds() - start, std::memory_order_relaxed);
            framesRendered.fetch_add(1, std::memory_order_relaxed);
            frameAllocations.store(engine::allocationCount() - allocationsBefore,
                                   std::memory_order_relaxed);
        } else if (nowSeconds() - lastActivity > kLingerSeconds) {
            frames.setRunning(false);
        }
    }

    void run() {
        pthread_setname_np("geometry.engine");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        if (engine::allocationCountingEnabled()) {
            engine::startCountingAllocations();
        }
        @autoreleasepool {
            runLoop = CFRunLoopGetCurrent();
            CFRunLoopSourceContext context{};
            context.info = this;
            context.perform = [](void* info) {
                @autoreleasepool {
                    static_cast<GeoEngine*>(info)->drainCommands();
                }
            };
            source = CFRunLoopSourceCreate(nullptr, 0, &context);
            CFRunLoopAddSource(runLoop, source, kCFRunLoopCommonModes);
            ready.store(true, std::memory_order_release);
            ready.notify_one();
            while (!stopping) {
                CFRunLoopRun();
            }
            CFRunLoopRemoveSource(runLoop, source, kCFRunLoopCommonModes);
            CFRelease(source);
        }
    }
};

const char* geo_engine_version(void) { return "0.1.0"; }

GeoEngine* geo_engine_create(const GeoEngineDescriptor* descriptor) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    std::unique_ptr<gpu::Renderer> renderer =
        device ? gpu::Renderer::create(device, descriptor->shader_library_path) : nullptr;
    if (!renderer) {
        return nullptr;
    }
    auto* engine = new GeoEngine;
    engine->device = device;
    engine->renderer = std::move(renderer);
    engine->importPaths = {descriptor->import_helper_path, descriptor->cache_directory};
    engine->eventCallback = descriptor->event_callback;
    engine->eventContext = descriptor->event_context;

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    const int result = pthread_create(
        &engine->thread, &attributes,
        [](void* argument) -> void* {
            static_cast<GeoEngine*>(argument)->run();
            return nullptr;
        },
        engine);
    pthread_attr_destroy(&attributes);
    GEO_ASSERT(result == 0);
    engine->ready.wait(false, std::memory_order_acquire);
    return engine;
}

void geo_engine_destroy(GeoEngine* engine) {
    if (!engine) {
        return;
    }
    Command command{};
    command.kind = Command::Kind::Stop;
    engine->post(command);
    pthread_join(engine->thread, nullptr);
    delete engine;
}

void geo_engine_attach_layer(GeoEngine* engine, void* metal_layer) {
    Command command{};
    command.kind = Command::Kind::AttachLayer;
    command.layer = (__bridge_retained void*)(__bridge CAMetalLayer*)metal_layer;
    engine->post(command);
}

void geo_engine_resize(GeoEngine* engine, uint32_t width, uint32_t height, float scale) {
    Command command{};
    command.kind = Command::Kind::Resize;
    command.resize = {width, height, scale};
    engine->post(command);
}

void geo_engine_post_pointer(GeoEngine* engine, const GeoPointerEvent* event) {
    Command command{};
    command.kind = Command::Kind::Pointer;
    command.pointer = *event;
    engine->post(command);
}

void geo_engine_open_file(GeoEngine* engine, const char* path) {
    Command command{};
    command.kind = Command::Kind::OpenFile;
    command.path = strdup(path);
    engine->post(command);
}

void geo_engine_perform(GeoEngine* engine, GeoAction action) {
    Command command{};
    command.kind = Command::Kind::Action;
    command.action = action;
    engine->post(command);
}

void geo_engine_request_frame(GeoEngine* engine) {
    Command command{};
    command.kind = Command::Kind::RequestFrame;
    engine->post(command);
}

void geo_engine_report_latency(GeoEngine* engine) { engine->latency.report(); }

void geo_trace_memory(const char* label) {
    if (!traceEnabled()) {
        return;
    }
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    std::fprintf(stderr, "trace: %-28s footprint %7.1f MB, peak %7.1f MB\n", label,
                 double(info.phys_footprint) / 1e6, double(info.ledger_phys_footprint_peak) / 1e6);
}

GeoEngineStats geo_engine_stats(const GeoEngine* engine) {
    return {engine->framesRendered.load(std::memory_order_relaxed),
            engine->lastFrameSeconds.load(std::memory_order_relaxed),
            engine->frameAllocations.load(std::memory_order_relaxed),
            engine->publishedHover.load(std::memory_order_relaxed),
            engine->publishedSelection.load(std::memory_order_relaxed)};
}
