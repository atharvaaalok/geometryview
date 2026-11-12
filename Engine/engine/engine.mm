// Implements the C API on the Mac: the portable engine core (core/core.h) on its own thread, drawn
// with Metal on a display link, importing through helper processes.
#include "engine.h"

#include "base/assert.h"
#include "base/spsc_ring.h"
#include "base/time.h"
#include "base/trace.h"
#include "core/core.h"
#include "engine/allocation_counter.h"
#include "engine/latency_probe.h"
#include "engine/png.h"
#include "gpu/frame_source.h"
#include "gpu/renderer.h"
#include "ipc/cache_eviction.h"
#include "ipc/import_job.h"
#include "library/gate.h"
#include "model/name_index.h"
#include "model/visibility.h"
#include "nav/pointer_controller.h"
#include "render/style.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/proc_info.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>

namespace {

using namespace geo;

constexpr usize kCommandCapacity = 4096;
// The frame whose allocations GEOMETRYVIEW_ALLOCS=stacks prints: one well past startup.
constexpr u64 kStacksFrame = 300;

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

struct NodeCommand {
    u32 document, node;
    bool extend;
    GeoNodeAction action;
};

struct Command {
    enum class Kind : u8 {
        AttachLayer,
        Resize,
        Pointer,
        OpenFile,
        AddFile,
        Close,
        Action,
        SetMode,
        SetTool,
        SelectNode,
        HoverNode,
        NodeAction,
        SetInsets,
        SetOverlay,
        SetLook,
        SetSection,
        SetMouse,
        Snapshot,
        RequestFrame,
        SetCovered,
        Stop
    };
    Kind kind;
    union {
        void* layer;
        ResizeCommand resize;
        GeoPointerEvent pointer;
        char* path; // owned, released by the engine thread
        GeoAction action;
        GeoSelectionMode mode;
        GeoTool tool;
        NodeCommand node;
        nav::Insets insets;
        bool shown;
        GeoLook look;
        GeoSection section;
        GeoMouseSettings mouse;
    };
};

} // namespace

/// A document's tree, shared with the app's thread. It holds the model's mapping.
struct GeoModel {
    std::shared_ptr<scene::Segment> segment;
    std::unique_ptr<model::NameIndex> names; // built on the first search
};

/// The engine on the Mac: the portable core on its own thread, with Metal, a display link and
/// import helper processes. The main thread only ever enqueues; the engine thread drains the
/// queue, which makes it the single writer of the core's state and removes most locking.
struct GeoEngine final : engine::Host {
    SpscRing<Command, kCommandCapacity> commands;
    CFRunLoopRef runLoop = nullptr;
    CFRunLoopSourceRef source = nullptr;
    CFRunLoopTimerRef refineTimer = nullptr;
    pthread_t thread{};
    std::atomic<bool> ready{false};
    bool stopping = false;

    id<MTLDevice> device = nil;
    std::unique_ptr<gpu::Renderer> renderer;
    gpu::DisplayLinkFrameSource frames;
    bool framesWanted = false;
    bool covered = false;
    std::unique_ptr<engine::Core> core;

    ipc::ImportPaths importPaths;
    GeoEventCallback eventCallback = nullptr;
    void* eventContext = nullptr;
    // Imports in flight, by document token, and the refinement, so closing can cancel them.
    std::unordered_map<u64, std::shared_ptr<ipc::ImportCancel>> imports;
    std::shared_ptr<ipc::ImportCancel> refineCancel;

    const bool traceLaunch = traceEnabled();
    engine::LatencyProbe latency;

    // What the app's thread reads.
    std::atomic<u64> framesRendered{0};
    std::atomic<double> lastFrameSeconds{0.0};
    std::atomic<u64> frameAllocations{0};
    std::atomic<u64> frameOwnAllocations{0};
    std::atomic<u32> publishedHover{0};
    std::atomic<u32> selectedCount{0};
    std::atomic<u32> hiddenCount{0};
    std::mutex summaryMutex;
    GeoSelectionSummary summary{0, 0, 0, GEO_NO_NODE, GeoSelectFaces};
    std::mutex modelsMutex;
    std::vector<std::shared_ptr<scene::Segment>> publishedModels;
    // Swatch colours: one per occurrence in global order, and each document's first occurrence.
    std::mutex swatchMutex;
    std::vector<u32> publishedSwatches;
    std::vector<u32> swatchBases;
    std::mutex nodeStateMutex;
    std::vector<std::vector<model::NodeVisibility>> publishedNodeStates;

    void post(const Command& command) {
        if (commands.tryPush(command)) {
            CFRunLoopSourceSignal(source);
            CFRunLoopWakeUp(runLoop);
        }
    }

    // Runs `work` on the engine thread.
    template <typename Work> void onEngineThread(Work work) {
        CFRunLoopPerformBlock(runLoop, kCFRunLoopCommonModes, ^{
            @autoreleasepool {
                work();
            }
        });
        CFRunLoopWakeUp(runLoop);
    }

    // ---- engine::Host --------------------------------------------------------------------------

    double now() const override { return nowSeconds(); }

    bool reduceMotion() const override {
        return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
    }

    // The core asks for frames; they run only while the view is not covered, as by the gallery.
    void runFrames(bool running) override {
        framesWanted = running;
        frames.setRunning(running && !covered);
    }

    void scheduleRefinement(double delaySeconds) override {
        if (refineTimer != nullptr) {
            CFRunLoopTimerSetNextFireDate(refineTimer, CFAbsoluteTimeGetCurrent() + delaySeconds);
        }
    }

    // Delivers import results to the engine thread, tagged so a closed document's are ignored.
    class ImportBridge final : public ipc::ImportListener {
    public:
        ImportBridge(GeoEngine* engine, u64 token) : engine_(engine), token_(token) {}

        void segmentReady(std::shared_ptr<scene::Segment> segment) override {
            GeoEngine* engine = engine_;
            const u64 token = token_;
            engine->onEngineThread([=] { engine->core->addSegment(token, segment); });
        }
        void progress(u32 done, u32 total) override {
            GeoEngine* engine = engine_;
            const u64 token = token_;
            engine->onEngineThread([=] { engine->core->importProgress(token, done, total); });
        }
        void warning(const std::string& text) override {
            std::fprintf(stderr, "import: %s\n", text.c_str());
        }

    private:
        GeoEngine* engine_;
        u64 token_;
    };

    static engine::ImportResult resultOf(const ipc::ImportOutcome& outcome) {
        return {outcome.ok, outcome.fromCache, outcome.message, outcome.blobPath};
    }

    void startImport(u64 token, const std::string& path) override {
        const std::shared_ptr<ipc::ImportCancel> cancel = std::make_shared<ipc::ImportCancel>();
        imports[token] = cancel;
        const ipc::ImportPaths paths = importPaths;
        const std::string file = path; // by value: the block outlives the caller's string
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            // Granted at once; gallery imports start nothing more until it is done.
            const library::ImportGate::Ticket ticket = library::ImportGate::shared().open();
            ImportBridge bridge(this, token);
            const ipc::ImportOutcome outcome = ipc::importFile(file, paths, bridge, cancel.get());
            onEngineThread([=, this] {
                imports.erase(token);
                core->importFinished(token, resultOf(outcome));
            });
        });
    }

    void startRefinement(u64 token, const std::string& path, u32 level,
                         std::vector<u32> definitions) override {
        refineCancel = std::make_shared<ipc::ImportCancel>();
        const std::shared_ptr<ipc::ImportCancel> cancel = refineCancel;
        const ipc::ImportPaths paths = importPaths;
        const std::string file = path;
        const double started = nowSeconds();
        const u32 count = u32(definitions.size());
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            const library::ImportGate::Ticket ticket = library::ImportGate::shared().open();
            ImportBridge bridge(this, token);
            const ipc::ImportOutcome outcome =
                ipc::refineFile(file, paths, level, definitions, bridge, cancel.get());
            onEngineThread([=, this] {
                core->refinementFinished(token, outcome.ok, count, level, started);
            });
        });
    }

    void cancelImport(u64 token) override {
        if (const auto it = imports.find(token); it != imports.end()) {
            it->second->cancel();
            imports.erase(it);
        }
    }

    void cancelRefinement() override {
        if (refineCancel) {
            refineCancel->cancel();
        }
    }

    // Keeps the cache within its budget, on a background thread, sparing what is open.
    void evictCache(std::vector<std::string> keep) override {
        const std::string directory = importPaths.cacheDirectory;
        __block std::vector<std::string> spared = std::move(keep);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            ipc::evictCache(directory, ipc::cacheBudget(directory), spared);
        });
    }

    void emit(const GeoEvent& event) override {
        if (eventCallback != nullptr) {
            eventCallback(eventContext, &event);
        }
    }

    void publishModel(u32 document, std::shared_ptr<scene::Segment> model) override {
        std::lock_guard lock(modelsMutex);
        publishedModels.resize(std::max<usize>(publishedModels.size(), document + 1));
        publishedModels[document] = std::move(model);
    }

    void clearModels() override {
        std::lock_guard lock(modelsMutex);
        publishedModels.clear();
    }

    void publishSwatches(std::vector<u32> swatches, std::vector<u32> bases) override {
        std::lock_guard lock(swatchMutex);
        publishedSwatches = std::move(swatches);
        swatchBases = std::move(bases);
    }

    void publishNodeStates(std::vector<std::vector<model::NodeVisibility>> states) override {
        std::lock_guard lock(nodeStateMutex);
        publishedNodeStates = std::move(states);
    }

    void publishSummary(const engine::Summary& counts) override {
        selectedCount.store(counts.selectedOccurrences, std::memory_order_relaxed);
        hiddenCount.store(counts.hiddenOccurrences, std::memory_order_relaxed);
        std::lock_guard lock(summaryMutex);
        summary = counts.selection;
    }

    void traceMemory(const char* label) override { geo_trace_memory(label); }

    // ---- The engine thread ---------------------------------------------------------------------

    void drainCommands() {
        bool pointerMoved = false;
        while (auto command = commands.tryPop()) {
            switch (command->kind) {
            case Command::Kind::AttachLayer:
                attachLayer((__bridge_transfer CAMetalLayer*)command->layer);
                break;
            case Command::Kind::Resize:
                core->resize(command->resize.width, command->resize.height, command->resize.scale);
                break;
            case Command::Kind::Pointer:
                core->pointer(command->pointer);
                pointerMoved = true;
                break;
            case Command::Kind::OpenFile:
            case Command::Kind::AddFile:
                core->openFile(command->path, command->kind == Command::Kind::AddFile);
                std::free(command->path);
                break;
            case Command::Kind::Close:
                core->close();
                break;
            case Command::Kind::Action:
                core->perform(command->action);
                break;
            case Command::Kind::SetMode:
                core->setSelectionMode(command->mode);
                break;
            case Command::Kind::SetTool:
                core->setTool(command->tool);
                break;
            case Command::Kind::SelectNode:
                core->selectNode(command->node.document, command->node.node, command->node.extend);
                break;
            case Command::Kind::NodeAction:
                core->nodeAction(command->node.document, command->node.node, command->node.action);
                break;
            case Command::Kind::HoverNode:
                core->hoverNode(command->node.document, command->node.node);
                break;
            case Command::Kind::SetMouse:
                core->setMouse(command->mouse);
                break;
            case Command::Kind::SetSection:
                core->setSection(command->section);
                break;
            case Command::Kind::SetLook:
                core->setLook(command->look);
                break;
            case Command::Kind::SetInsets:
                core->setInsets(command->insets);
                break;
            case Command::Kind::SetOverlay:
                core->setOverlayShown(command->shown);
                break;
            case Command::Kind::Snapshot:
                writeSnapshot(command->path);
                std::free(command->path);
                break;
            case Command::Kind::RequestFrame:
                core->requestFrame();
                break;
            case Command::Kind::SetCovered:
                covered = command->shown;
                frames.setRunning(framesWanted && !covered);
                if (!covered) {
                    core->requestFrame(); // what changed meanwhile
                }
                break;
            case Command::Kind::Stop:
                stopping = true;
                CFRunLoopStop(runLoop);
                break;
            }
        }
        if (pointerMoved) {
            core->pointerSettled();
        }
    }

    // Draws the current view offscreen and saves it, written to a temporary name first so the file
    // only ever appears whole.
    void writeSnapshot(const std::string& path) {
        const std::vector<u8> pixels = core->snapshot();
        const nav::Viewport viewport = core->viewport();
        const std::string temporary = path + ".partial";
        if (engine::writePng(temporary.c_str(), pixels, u32(viewport.width),
                             u32(viewport.height))) {
            rename(temporary.c_str(), path.c_str());
        }
    }

    void attachLayer(CAMetalLayer* layer) {
        renderer->attachLayer(layer);
        frames.attach(layer, [this](const gpu::FrameTick& tick) { onFrame(tick); });
        core->requestFrame();
    }

    void onFrame(const gpu::FrameTick& tick) {
        const std::optional<engine::Frame> frame = core->frame();
        if (!frame) {
            return;
        }
        publishedHover.store(core->hoveredId(), std::memory_order_relaxed);
        if (traceLaunch && framesRendered.load(std::memory_order_relaxed) == 0) {
            [tick.drawable addPresentedHandler:^(id<MTLDrawable> drawable) {
                (void)drawable;
                std::fprintf(stderr, "trace: first frame presented %.1f ms after process start\n",
                             secondsSinceProcessStart() * 1e3);
                geo_trace_memory("first frame presented");
            }];
        }
        if (latency.enabled() && frame->inputTime != 0.0) {
            latency.track(tick.drawable, frame->inputTime, frame->started, tick.commitDeadline,
                          tick.presentationTime);
        }
        // GEOMETRYVIEW_ALLOCS=stacks prints where a frame well past startup allocates.
        const bool printStacks = framesRendered.load(std::memory_order_relaxed) == kStacksFrame;
        if (printStacks) {
            engine::keepAllocationStacks();
        }
        const u64 allocationsBefore = engine::allocationCount();
        const u64 ownBefore = engine::ownAllocationCount();
        renderer->render(tick.drawable, frame->view, &frame->overlay);
        lastFrameSeconds.store(nowSeconds() - frame->started, std::memory_order_relaxed);
        framesRendered.fetch_add(1, std::memory_order_relaxed);
        frameAllocations.store(engine::allocationCount() - allocationsBefore,
                               std::memory_order_relaxed);
        frameOwnAllocations.store(engine::ownAllocationCount() - ownBefore,
                                  std::memory_order_relaxed);
        if (printStacks) {
            engine::printAllocationStacks(STDERR_FILENO);
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
            refineTimer = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 1e9,
                                                          1e9, 0, 0, ^(CFRunLoopTimerRef) {
                                                              @autoreleasepool {
                                                                  core->refinementDue();
                                                              }
                                                          });
            CFRunLoopAddTimer(runLoop, refineTimer, kCFRunLoopCommonModes);
            ready.store(true, std::memory_order_release);
            ready.notify_one();
            while (!stopping) {
                CFRunLoopRun();
            }
            CFRunLoopRemoveSource(runLoop, source, kCFRunLoopCommonModes);
            CFRelease(source);
            CFRunLoopTimerInvalidate(refineTimer);
            CFRelease(refineTimer);
        }
    }
};

const char* geo_engine_version(void) { return "0.3.0"; }

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
    engine->core = std::make_unique<engine::Core>(*engine->renderer, *engine);
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

void geo_engine_add_file(GeoEngine* engine, const char* path) {
    Command command{};
    command.kind = Command::Kind::AddFile;
    command.path = strdup(path);
    engine->post(command);
}

void geo_engine_close(GeoEngine* engine) {
    Command command{};
    command.kind = Command::Kind::Close;
    engine->post(command);
}

void geo_engine_perform(GeoEngine* engine, GeoAction action) {
    Command command{};
    command.kind = Command::Kind::Action;
    command.action = action;
    engine->post(command);
}

void geo_engine_set_selection_mode(GeoEngine* engine, GeoSelectionMode mode) {
    Command command{};
    command.kind = Command::Kind::SetMode;
    command.mode = mode;
    engine->post(command);
}

void geo_engine_set_tool(GeoEngine* engine, GeoTool tool) {
    Command command{};
    command.kind = Command::Kind::SetTool;
    command.tool = tool;
    engine->post(command);
}

void geo_engine_select_node(GeoEngine* engine, uint32_t document, uint32_t node, bool extend) {
    Command command{};
    command.kind = Command::Kind::SelectNode;
    command.node = {document, node, extend, GeoNodeHide};
    engine->post(command);
}

void geo_engine_node_action(GeoEngine* engine, uint32_t document, uint32_t node,
                            GeoNodeAction action) {
    Command command{};
    command.kind = Command::Kind::NodeAction;
    command.node = {document, node, false, action};
    engine->post(command);
}

static_assert(u32(model::NodeVisibility::Hidden) == GeoNodeHidden &&
              u32(model::NodeVisibility::Ghosted) == GeoNodeGhosted &&
              u32(model::NodeVisibility::Mixed) == GeoNodeMixed);

GeoNodeState geo_engine_node_state(GeoEngine* engine, uint32_t document, uint32_t node) {
    std::lock_guard lock(engine->nodeStateMutex);
    if (document >= engine->publishedNodeStates.size() ||
        node >= engine->publishedNodeStates[document].size()) {
        return GeoNodeShown;
    }
    return GeoNodeState(engine->publishedNodeStates[document][node]);
}

void geo_engine_hover_node(GeoEngine* engine, uint32_t document, uint32_t node) {
    Command command{};
    command.kind = Command::Kind::HoverNode;
    command.node = {document, node, false, GeoNodeHide};
    engine->post(command);
}

void geo_engine_set_look(GeoEngine* engine, const GeoLook* look) {
    Command command{};
    command.kind = Command::Kind::SetLook;
    command.look = *look;
    engine->post(command);
}

GeoMouseSettings geo_default_mouse_settings(void) {
    GeoMouseSettings settings{};
    const auto defaults = nav::defaultMouseBindings();
    for (const nav::MouseBinding& b : defaults) {
        const u32 button = b.button == nav::Button::Left     ? GeoButtonLeft
                           : b.button == nav::Button::Right  ? GeoButtonRight
                           : b.button == nav::Button::Middle ? GeoButtonMiddle
                                                             : GeoButtonNone;
        settings.bindings[settings.binding_count++] = {button, b.modifiers, GeoGesture(b.gesture)};
    }
    settings.zoom_toward_pointer = true;
    settings.invert_wheel = false;
    settings.zoom_speed = 1.0f;
    return settings;
}

void geo_engine_set_mouse(GeoEngine* engine, const GeoMouseSettings* settings) {
    Command command{};
    command.kind = Command::Kind::SetMouse;
    command.mouse = *settings;
    engine->post(command);
}

void geo_engine_set_section(GeoEngine* engine, const GeoSection* section) {
    Command command{};
    command.kind = Command::Kind::SetSection;
    command.section = *section;
    engine->post(command);
}

void geo_engine_set_view_insets(GeoEngine* engine, float left, float top, float right,
                                float bottom) {
    Command command{};
    command.kind = Command::Kind::SetInsets;
    command.insets = {left, top, right, bottom};
    engine->post(command);
}

void geo_engine_set_overlay_shown(GeoEngine* engine, bool shown) {
    Command command{};
    command.kind = Command::Kind::SetOverlay;
    command.shown = shown;
    engine->post(command);
}

void geo_engine_write_snapshot(GeoEngine* engine, const char* path) {
    Command command{};
    command.kind = Command::Kind::Snapshot;
    command.path = strdup(path);
    engine->post(command);
}

uint32_t geo_engine_swatch(GeoEngine* engine, uint32_t document, uint32_t occurrence) {
    std::lock_guard lock(engine->swatchMutex);
    if (document >= engine->swatchBases.size()) {
        return 0;
    }
    const u64 global = u64(engine->swatchBases[document]) + occurrence;
    return global < engine->publishedSwatches.size() ? engine->publishedSwatches[global] : 0;
}

void geo_engine_set_covered(GeoEngine* engine, bool covered) {
    Command command{};
    command.kind = Command::Kind::SetCovered;
    command.shown = covered;
    engine->post(command);
}

void geo_engine_request_frame(GeoEngine* engine) {
    Command command{};
    command.kind = Command::Kind::RequestFrame;
    engine->post(command);
}

void geo_engine_report_latency(GeoEngine* engine) { engine->latency.report(); }

GeoModel* geo_engine_copy_model(GeoEngine* engine, uint32_t document) {
    std::lock_guard lock(engine->modelsMutex);
    if (document >= engine->publishedModels.size() || !engine->publishedModels[document]) {
        return nullptr;
    }
    auto* model = new GeoModel;
    model->segment = engine->publishedModels[document];
    return model;
}

void geo_model_release(GeoModel* model) { delete model; }

uint32_t geo_model_node_count(const GeoModel* model) {
    return uint32_t(model->segment->view().nodes.size());
}

const GeoNode* geo_model_nodes(const GeoModel* model) {
    static_assert(sizeof(GeoNode) == sizeof(format::NodeRecord));
    return reinterpret_cast<const GeoNode*>(model->segment->view().nodes.data());
}

const char* geo_model_names(const GeoModel* model) { return model->segment->view().strings.data(); }

uint32_t geo_model_search(GeoModel* model, const char* query, uint32_t* results,
                          uint32_t capacity) {
    if (!model->names) {
        model->names = std::make_unique<model::NameIndex>(model->segment->view());
    }
    return model->names->search(query, std::span(results, capacity));
}

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

GeoSelectionSummary geo_engine_selection(GeoEngine* engine) {
    std::lock_guard lock(engine->summaryMutex);
    return engine->summary;
}

GeoEngineStats geo_engine_stats(const GeoEngine* engine) {
    return {engine->framesRendered.load(std::memory_order_relaxed),
            engine->lastFrameSeconds.load(std::memory_order_relaxed),
            engine->frameAllocations.load(std::memory_order_relaxed),
            engine->frameOwnAllocations.load(std::memory_order_relaxed),
            engine->publishedHover.load(std::memory_order_relaxed),
            engine->selectedCount.load(std::memory_order_relaxed),
            engine->hiddenCount.load(std::memory_order_relaxed),
            engine->renderer ? engine->renderer->meanGpuSeconds() : 0.0,
            engine->renderer ? engine->renderer->gpuPercentileSeconds(0.99) : 0.0};
}
