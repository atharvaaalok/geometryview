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
#include "engine/look_state.h"
#include "engine/png.h"
#include "gpu/frame_source.h"
#include "gpu/renderer.h"
#include "ipc/cache_eviction.h"
#include "ipc/import_job.h"
#include "model/interaction.h"
#include "model/name_index.h"
#include "model/visibility.h"
#include "nav/camera.h"
#include "nav/flight.h"
#include "nav/orbit_policy.h"
#include "nav/pointer_controller.h"
#include "render/overlay.h"
#include "render/refinement.h"
#include "render/shell.h"
#include "render/view.h"
#include "scene/scene.h"

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

namespace {

using namespace geo;

// ProMotion drops its refresh rate when presents stop, so frames keep flowing briefly after the
// last input, which keeps the next interaction at full rate.
constexpr double kLingerSeconds = 0.25;
constexpr usize kCommandCapacity = 4096;
constexpr double kFitMargin = 1.12;
constexpr double kDefaultAzimuth = 35.0 * M_PI / 180.0;
constexpr double kDefaultElevation = 25.0 * M_PI / 180.0;
// Refinement waits for the view to settle, so a drag never starts work it would then outrun.
constexpr double kRefineDelaySeconds = 0.3;
// Camera flights, as to a standard view, take this long unless motion is reduced.
constexpr double kFlightSeconds = 0.55;
// See-through fades in and out over this long, as in Colorfigures.
constexpr double kSeeThroughSeconds = 0.7;
// A scene this heavy leaves out edges and the shadow while the view moves, and draws them once it
// settles: spike 5 found 30M triangles held 120 Hz without them and 48 fps with them.
constexpr u64 kHeavyTriangles = 20'000'000;
constexpr u64 kHeavyEdgeSegments = 2'000'000;
// The square views that classify the shell, in pixels.
constexpr u32 kShellViewSize = 160;

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
        Action,
        SetMode,
        SetTool,
        SelectNode,
        HoverNode,
        NodeAction,
        SetInsets,
        SetLook,
        SetSection,
        SetMouse,
        Snapshot,
        RequestFrame,
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
        GeoLook look;
        GeoSection section;
        GeoMouseSettings mouse;
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

model::Tool toTool(GeoTool tool) {
    switch (tool) {
    case GeoToolHide:
        return model::Tool::Hide;
    case GeoToolGhost:
        return model::Tool::Ghost;
    case GeoToolSelect:
        return model::Tool::Select;
    }
    return model::Tool::Select;
}

model::SelectionMode toMode(GeoSelectionMode mode) {
    switch (mode) {
    case GeoSelectBodies:
        return model::SelectionMode::Body;
    case GeoSelectParts:
        return model::SelectionMode::Part;
    case GeoSelectFaces:
        return model::SelectionMode::Face;
    }
    return model::SelectionMode::Face;
}

} // namespace

/// A document's tree, shared with the app's thread. It holds the model's mapping.
struct GeoModel {
    std::shared_ptr<scene::MappedSegment> segment;
    std::unique_ptr<model::NameIndex> names; // built on the first search
};

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

    // One per open file, parallel to the scene's documents. Results carry their document's token,
    // so those of a closed document are dropped.
    struct OpenDocument {
        u64 token = 0;
        std::string path;
        bool importing = true;
        double started = 0.0;
        std::shared_ptr<ipc::ImportCancel> cancel;
        std::vector<u32> requestedLevels; // the finest level asked for, per definition
        std::string blobPath;             // its cache file, once known
    };
    std::vector<OpenDocument> documents;
    u64 nextToken = 1;

    scene::Scene scene;
    model::Interaction interaction{scene};
    Aabb bounds{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
    bool sceneChanged = false;
    bool hasFramed = false;
    bool userMovedCamera = false;
    // What framing and rotation treat as the model: everything not wholly hidden.
    Aabb visible{};
    u64 seenVisibility = ~u64(0);
    // The view to return to when an isolate ends, kept from the first of nested isolates.
    std::optional<nav::Camera> cameraBeforeIsolate;

    // Models published for the app's thread, which reads trees and searches them.
    std::mutex modelsMutex;
    std::vector<std::shared_ptr<scene::MappedSegment>> publishedModels;

    // Finer geometry for what the view shows closely. One job runs at a time.
    CFRunLoopTimerRef refineTimer = nullptr;
    double lastNavigation = -1e9;
    bool detailReduced = false; // the last frame left out edges and the shadow
    bool refining = false;
    std::shared_ptr<ipc::ImportCancel> refineCancel;

    nav::Camera camera;
    nav::Flight flight;
    nav::Viewport viewport;
    float pixelsPerPoint = 2.0f;
    nav::Insets insetPoints; // as the app gives them
    nav::Insets insets;      // in pixels
    engine::LookState look;
    render::Section section;
    // See-through: k moves from where it was toward the target over kSeeThroughSeconds.
    float seeThrough = 0.0f;
    float seeThroughFrom = 0.0f;
    float seeThroughTarget = 0.0f;
    double seeThroughStart = 0.0;
    u32 cubeHover = 0;   // face index plus one, or zero
    u32 cubePressed = 0; // the face a press began on, plus one
    nav::OrbitPolicy orbit;
    nav::PointerController pointer;
    render::DepthDecoder depthDecoder;

    float cursorX = -1, cursorY = -1;
    bool cursorInside = false;
    u32 hoveredId = 0;
    std::pair<u32, u32> hover{0, 0};
    std::optional<model::NodeRef> hoveredNode;
    GeoTool tool = GeoToolSelect;
    u64 publishedStateVersion = ~u64(0);

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
    std::atomic<u32> selectedCount{0};
    std::atomic<u32> hiddenCount{0};
    std::mutex summaryMutex;
    // Swatch colours published for the app's thread: one per occurrence in global order, and the
    // global index of each document's first occurrence.
    std::mutex swatchMutex;
    std::vector<u32> publishedSwatches;
    std::vector<u32> swatchBases;
    // Each tree node's state, per document, published for the app's thread.
    std::mutex nodeStateMutex;
    std::vector<std::vector<model::NodeVisibility>> publishedNodeStates;
    GeoSelectionSummary summary{0, 0, 0, GEO_NO_NODE, GeoSelectFaces};
    std::optional<std::pair<model::NodeRef, u32>> hoveredPart;

    GeoEngine()
        : pointer(camera, viewport, orbit, [this](double x, double y) { return pickAt(x, y); }) {}

    void post(const Command& command) {
        if (commands.tryPush(command)) {
            CFRunLoopSourceSignal(source);
            CFRunLoopWakeUp(runLoop);
        }
    }

    void emit(GeoEventKind kind, const char* message = "", u32 document = 0, u32 node = GEO_NO_NODE,
              u32 done = 0, u32 total = 0) {
        if (traceLaunch && kind != GeoEventProgress) {
            std::fprintf(stderr, "trace: event %d: %s\n", int(kind), message);
        }
        if (eventCallback != nullptr) {
            const GeoEvent event{kind, document, node, done, total, message};
            eventCallback(eventContext, &event);
        }
    }

    void requestFrame() {
        needsFrame = true;
        lastActivity = nowSeconds();
        frames.setRunning(true);
    }

    // The view cube's face under a pixel, plus one, or zero.
    u32 cubeFaceAt(double x, double y) const {
        const std::optional<gpu::PixelHit> pixel =
            renderer->readPixel(int(x), int(y), depthDecoder);
        return pixel && pixel->id >= kCubeIdBase ? pixel->id - kCubeIdBase + 1 : 0;
    }

    // The model's surface under a pixel. The view cube is not a surface: it is no pivot to turn
    // about, and clicking it is handled on its own.
    std::optional<nav::SurfaceHit> pickAt(double x, double y) {
        const std::optional<gpu::PixelHit> pixel =
            renderer->readPixel(int(x), int(y), depthDecoder);
        if (!pixel || pixel->id >= kCubeIdBase) {
            return std::nullopt;
        }
        const double ndcX = 2.0 * x / viewport.width - 1.0;
        const double ndcY = 1.0 - 2.0 * y / viewport.height;
        return nav::SurfaceHit{pixel->id, camera.unproject(ndcX, ndcY, pixel->viewDepth, viewport),
                               pixel->viewDepth};
    }

    // What hover lights up: a tree row the pointer is over, or else the surface under the pointer.
    void refreshHover() {
        const std::pair<u32, u32> range =
            hoveredNode ? interaction.nodeRange(*hoveredNode) : interaction.hoverRange(hoveredId);
        if (range != hover) {
            hover = range;
            requestFrame();
        }
        // The app labels what is under the pointer, so it hears when that changes, not per frame.
        std::optional<std::pair<model::NodeRef, u32>> part;
        if (!hoveredNode) {
            part = interaction.partAt(hoveredId);
        }
        if (part != hoveredPart) {
            hoveredPart = part;
            if (part) {
                emit(GeoEventHoverChanged, "", part->first.document, part->first.node,
                     part->second);
            } else {
                emit(GeoEventHoverChanged, "", 0, GEO_NO_NODE);
            }
        }
    }

    // Reading the ID buffer costs nanoseconds and no GPU work. A frame is drawn only if the surface
    // under the pointer actually changed.
    void updateHover() {
        if (pointer.isPressed()) {
            return;
        }
        u32 id = 0;
        u32 cube = 0;
        if (cursorInside) {
            cube = cubeFaceAt(cursorX, cursorY);
            if (const std::optional<nav::SurfaceHit> hit = pickAt(cursorX, cursorY)) {
                id = hit->id;
            }
        }
        if (cube != cubeHover) {
            cubeHover = cube;
            requestFrame();
        }
        if (id != hoveredId) {
            hoveredId = id;
            refreshHover();
        }
    }

    // Flies the camera to a standard view of everything shown.
    void flyTo(nav::StandardView view) {
        if (!scene.hasGeometry()) {
            return;
        }
        flyTo(nav::standardView(camera, view, visible, viewport, kFitMargin, insets));
    }

    // Flies to frame what is selected, or everything if nothing is.
    void fitSelection() {
        const std::optional<Aabb> selected = selectionBounds();
        if (!selected) {
            perform(GeoActionFit);
            return;
        }
        flyToFit(*selected);
    }

    void flyToFit(const Aabb& box) {
        nav::Camera target = camera;
        nav::fit(target, box, viewport, kFitMargin, insets);
        flyTo(target);
    }

    void flyTo(const nav::Camera& target) {
        flight.start(camera, target, nowSeconds(), reduceMotion() ? 0.0 : kFlightSeconds);
        requestFrame();
    }

    // Bounds of the occurrences with anything selected, from their definitions' bounds.
    std::optional<Aabb> selectionBounds() const {
        const model::FaceState& state = interaction.state();
        return model::boundsWhere(
            scene, [&](u32 occurrence) { return state.anyFace(occurrence, kStateSelected); });
    }

    static bool reduceMotion() {
        return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
    }

    // After anything changes what is hidden, ghosted or selected.
    void stateChanged() {
        refreshHover();
        requestFrame();
        publishState();
        if (interaction.state().visibilityVersion() != seenVisibility) {
            visibilityChanged();
        }
        if (const std::optional<model::NodeRef> focus = interaction.focus()) {
            emit(GeoEventSelectionChanged, "", focus->document, focus->node);
        } else {
            emit(GeoEventSelectionChanged, "", 0, GEO_NO_NODE);
        }
    }

    // Hands the app each occurrence's swatch colour when the looks change: rarely, never per frame.
    void publishSwatches() {
        if (!look.swatchesChanged()) {
            return;
        }
        std::vector<u32> swatches = look.takeSwatches();
        std::vector<u32> bases(scene.documentCount());
        for (u32 d = 0; d < bases.size(); ++d) {
            bases[d] = scene.occurrenceBase(d);
        }
        {
            std::lock_guard lock(swatchMutex);
            publishedSwatches = std::move(swatches);
            swatchBases = std::move(bases);
        }
        emit(GeoEventLooksChanged);
    }

    // After what is shown changes, by hiding, ghosting or opening files: rotation and framing
    // follow what is shown, as though it were all that had been opened, and the end of an isolate
    // returns the view it began from.
    void visibilityChanged() {
        const model::FaceState& state = interaction.state();
        seenVisibility = state.visibilityVersion();
        visible = model::visibleBounds(scene, state).value_or(bounds);
        orbit.setVisibleBounds(visible);
        u32 hidden = 0, ghosted = 0;
        for (u32 o = 0; o < state.occurrenceCount(); ++o) {
            hidden += state.anyFace(o, kStateHidden) ? 1 : 0;
            ghosted += state.anyFace(o, kStateGhost) ? 1 : 0;
        }
        if (cameraBeforeIsolate && hidden == 0) {
            flyTo(*cameraBeforeIsolate);
            cameraBeforeIsolate.reset();
        }
        std::vector<std::vector<model::NodeVisibility>> states(scene.documentCount());
        for (u32 d = 0; d < states.size(); ++d) {
            states[d] = model::nodeStates(scene, state, d);
        }
        {
            std::lock_guard lock(nodeStateMutex);
            publishedNodeStates = std::move(states);
        }
        emit(GeoEventVisibilityChanged, "", 0, GEO_NO_NODE, hidden, ghosted);
    }

    // A tree row's button.
    void nodeAction(model::NodeRef node, GeoNodeAction action) {
        switch (action) {
        case GeoNodeHide:
            interaction.setNode(node, kStateHidden, true);
            break;
        case GeoNodeShow:
            interaction.setNode(node, kStateHidden, false);
            break;
        case GeoNodeGhost:
            interaction.setNode(node, kStateGhost, true);
            break;
        case GeoNodeUnghost:
            interaction.setNode(node, kStateGhost, false);
            break;
        case GeoNodeIsolate: {
            const u64 before = interaction.state().visibilityVersion();
            interaction.isolateNode(node);
            stateChanged();
            if (interaction.state().visibilityVersion() != before) {
                frameIsolated();
            }
            return;
        }
        }
        stateChanged();
    }

    // Isolating frames what is kept, as if it alone had been opened, and remembers the view to
    // return to when everything is shown again.
    void frameIsolated() {
        if (!cameraBeforeIsolate) {
            cameraBeforeIsolate = flight.active() ? flight.destination() : camera;
        }
        flyToFit(visible);
    }

    void handlePointer(const GeoPointerEvent& event) {
        lastActivity = nowSeconds();
        if (event.kind == GeoPointerLeave) {
            cursorInside = false;
            updateHover();
            return;
        }
        if (event.kind == GeoPointerCancel) {
            pointer.cancel();
            cubePressed = 0;
            updateHover();
            return;
        }
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
            flight.cancel();
            cubePressed = event.button == GeoButtonLeft ? cubeFaceAt(event.x, event.y) : 0;
            response = pointer.press(toButton(event.button), event.modifiers, event.x, event.y);
            break;
        case GeoPointerUp:
            response = pointer.release(toButton(event.button), event.x, event.y);
            break;
        case GeoPointerScroll:
        case GeoPointerMagnify:
            response = pointer.scroll(event.delta, event.x, event.y, event.timestamp);
            break;
        case GeoPointerLeave:
        case GeoPointerCancel:
            break;
        }

        if (response.cameraMoved) {
            userMovedCamera = true;
            scheduleRefinement();
            pendingInputTime = pendingInputTime == 0.0 ? event.timestamp : pendingInputTime;
            requestFrame();
        }
        if (response.clicked && cubePressed != 0) {
            flyTo(nav::StandardView(cubePressed - 1));
        } else if (response.clicked) {
            const bool extend =
                (event.modifiers & (nav::kModifierShift | nav::kModifierCommand)) != 0;
            interaction.click(response.clickedId.value_or(0), toTool(tool), extend);
            stateChanged();
        }
        if (event.kind == GeoPointerUp) {
            cubePressed = 0;
        }
    }

    void resetCamera() {
        const Vec3 direction = simd_make_double3(
            std::cos(kDefaultElevation) * std::sin(kDefaultAzimuth),
            -std::cos(kDefaultElevation) * std::cos(kDefaultAzimuth), std::sin(kDefaultElevation));
        camera.lookAt(bounds.center() + direction, bounds.center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, bounds, viewport, kFitMargin, insets);
    }

    void perform(GeoAction action) {
        lastActivity = nowSeconds();
        switch (action) {
        case GeoActionFit:
            if (scene.hasGeometry()) {
                nav::fit(camera, visible, viewport, kFitMargin, insets);
                orbit.navigated();
                scheduleRefinement();
            }
            break;
        case GeoActionToggleProjection:
            camera.perspective = !camera.perspective;
            if (scene.hasGeometry()) {
                nav::fit(camera, visible, viewport, kFitMargin, insets);
            }
            orbit.navigated();
            scheduleRefinement();
            break;
        case GeoActionClearSelection:
            interaction.clearSelection();
            break;
        case GeoActionShowAll:
            interaction.showAll();
            break;
        case GeoActionUnghostAll:
            interaction.unghostAll();
            break;
        case GeoActionIsolate: {
            const u64 before = interaction.state().visibilityVersion();
            interaction.isolate();
            stateChanged();
            if (interaction.state().visibilityVersion() != before) {
                frameIsolated();
            }
            return;
        }
        case GeoActionHideSelection:
            interaction.hideSelection();
            break;
        case GeoActionGhostSelection:
            interaction.ghostSelection();
            break;
        case GeoActionUndo:
            interaction.undo();
            break;
        case GeoActionRedo:
            interaction.redo();
            break;
        case GeoActionViewFront:
        case GeoActionViewBack:
        case GeoActionViewLeft:
        case GeoActionViewRight:
        case GeoActionViewTop:
        case GeoActionViewBottom:
        case GeoActionViewIso:
            flyTo(nav::StandardView(action - GeoActionViewFront));
            return;
        case GeoActionFitSelection:
            fitSelection();
            return;
        case GeoActionToggleSeeThrough:
            if (seeThroughTarget == 0.0f && !look.hasShells()) {
                classifyShells();
            }
            seeThroughFrom = seeThrough;
            seeThroughTarget = seeThroughTarget > 0.0f ? 0.0f : 1.0f;
            seeThroughStart = nowSeconds();
            if (reduceMotion()) {
                seeThrough = seeThroughTarget;
            }
            requestFrame();
            return;
        }
        stateChanged();
    }

    // Delivers import results to the engine thread, tagged so a closed document's are ignored.
    class ImportBridge final : public ipc::ImportListener {
    public:
        ImportBridge(GeoEngine* engine, u64 token) : engine_(engine), token_(token) {}

        void segmentReady(std::shared_ptr<scene::MappedSegment> segment) override {
            GeoEngine* engine = engine_;
            const u64 token = token_;
            engine->perform([=] { engine->addSegment(token, segment); });
        }
        void progress(u32 done, u32 total) override {
            GeoEngine* engine = engine_;
            const u64 token = token_;
            engine->perform([=] { engine->reportProgress(token, done, total); });
        }
        void warning(const std::string& text) override {
            std::fprintf(stderr, "import: %s\n", text.c_str());
        }

    private:
        GeoEngine* engine_;
        u64 token_;
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

    std::optional<u32> documentOf(u64 token) const {
        for (u32 d = 0; d < documents.size(); ++d) {
            if (documents[d].token == token) {
                return d;
            }
        }
        return std::nullopt;
    }

    void closeAll() {
        for (OpenDocument& document : documents) {
            if (document.cancel) {
                document.cancel->cancel();
            }
        }
        if (refineCancel) {
            refineCancel->cancel();
        }
        refining = false;
        documents.clear();
        scene.clear();
        interaction.reset();
        look.sceneChanged();
        {
            std::lock_guard lock(modelsMutex);
            publishedModels.clear();
        }
        hasFramed = false;
        userMovedCamera = false;
        cameraBeforeIsolate.reset();
        {
            std::lock_guard lock(nodeStateMutex);
            publishedNodeStates.clear();
        }
        hoveredId = 0;
        hoveredNode.reset();
        hover = {0, 0};
    }

    // By value: a block copies C++ objects it captures, but only the reference if given one, and
    // this block outlives the caller's string.
    void openFile(std::string path, bool add) {
        if (!add) {
            closeAll();
        }
        const std::string name = [@(path.c_str()) lastPathComponent].UTF8String;
        const u32 index = scene.addDocument();
        OpenDocument& document = documents.emplace_back();
        document.token = nextToken++;
        document.path = path;
        document.started = nowSeconds();
        document.cancel = std::make_shared<ipc::ImportCancel>();
        emit(GeoEventLoading, name.c_str(), index);
        sceneChanged = true;
        requestFrame();

        // The import can take seconds, and the engine thread must keep drawing.
        const u64 token = document.token;
        const std::shared_ptr<ipc::ImportCancel> cancel = document.cancel;
        const ipc::ImportPaths paths = importPaths;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            ImportBridge bridge(this, token);
            ipc::ImportOutcome outcome = ipc::importFile(path, paths, bridge, cancel.get());
            perform([=, this] { finishOpen(token, outcome); });
        });
    }

    void addSegment(u64 token, const std::shared_ptr<scene::MappedSegment>& segment) {
        const std::optional<u32> index = documentOf(token);
        if (!index) {
            return;
        }
        scene::Document& document = scene.document(*index);
        const bool hadModel = document.hasModel();
        std::string error;
        if (!document.add(segment, error)) {
            emit(GeoEventFailed, error.c_str(), *index);
            return;
        }
        sceneChanged = true;
        if (!hadModel && document.hasModel()) {
            interaction.sceneChanged();
            look.sceneChanged();
            publishModel(*index);
        }
        if (!hasFramed) {
            frameScene(true);
        } else if (!hadModel && document.hasModel()) {
            visibilityChanged(); // a file added to what is shown
        }
        requestFrame();
    }

    void publishModel(u32 index) {
        {
            std::lock_guard lock(modelsMutex);
            publishedModels.resize(std::max<usize>(publishedModels.size(), index + 1));
            for (const std::shared_ptr<scene::MappedSegment>& segment :
                 scene.document(index).segments()) {
                if (segment->view().hasModel()) {
                    publishedModels[index] = segment;
                }
            }
        }
        emit(GeoEventModelReady, "", index);
    }

    void reportProgress(u64 token, u32 done, u32 total) {
        if (const std::optional<u32> index = documentOf(token)) {
            emit(GeoEventProgress, "", *index, GEO_NO_NODE, done, total);
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
            nav::fit(camera, bounds, viewport, kFitMargin, insets);
        }
        visibilityChanged();
    }

    // Keeps the cache within its budget, on a background thread, sparing what is open.
    void evictCache() {
        std::vector<std::string> keep;
        for (const OpenDocument& open : documents) {
            if (!open.blobPath.empty()) {
                keep.push_back(open.blobPath);
            }
        }
        const std::string directory = importPaths.cacheDirectory;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            ipc::evictCache(directory, ipc::cacheBudget(directory), keep);
        });
    }

    void finishOpen(u64 token, const ipc::ImportOutcome& outcome) {
        const std::optional<u32> index = documentOf(token);
        if (!index) {
            return; // closed, or replaced by another file
        }
        OpenDocument& document = documents[*index];
        document.importing = false;
        scheduleRefinement();
        if (!outcome.ok) {
            emit(GeoEventFailed, outcome.message.c_str(), *index);
            return;
        }
        document.blobPath = outcome.blobPath;
        if (!outcome.fromCache) {
            evictCache();
        }
        frameScene(false); // estimated bounds become exact now that all geometry is here
        if (seeThroughTarget > 0.0f) {
            classifyShells(); // the shell is found again for the scene as it is now
        }
        requestFrame();
        if (traceLaunch) {
            std::fprintf(stderr, "trace: scene ready %.1f ms after opening\n",
                         (nowSeconds() - document.started) * 1e3);
        }
        if (std::none_of(documents.begin(), documents.end(),
                         [](const OpenDocument& open) { return open.importing; })) {
            emit(GeoEventSceneReady, "", *index);
        }
    }

    void applyInsets() {
        insets = {insetPoints.left * pixelsPerPoint, insetPoints.top * pixelsPerPoint,
                  insetPoints.right * pixelsPerPoint, insetPoints.bottom * pixelsPerPoint};
    }

    void scheduleRefinement() {
        lastNavigation = nowSeconds();
        if (refineTimer != nullptr) {
            CFRunLoopTimerSetNextFireDate(refineTimer,
                                          CFAbsoluteTimeGetCurrent() + kRefineDelaySeconds);
        }
    }

    // Asks for finer geometry where the settled view shows more than half a pixel of chord error.
    void refine() {
        if (refining) {
            return;
        }
        for (u32 index = 0; index < documents.size(); ++index) {
            if (!documents[index].importing && refineDocument(index)) {
                return;
            }
        }
    }

    bool refineDocument(u32 index) {
        const scene::Document& document = scene.document(index);
        if (!document.hasModel() || document.model().modelInfo.empty()) {
            return false;
        }
        const format::SegmentView& model = document.model();
        const format::ModelInfo& info = model.modelInfo[0];
        const render::RefinementPolicy policy{info.linearDeflection, info.maxLevel, 0.5};
        if (policy.linearDeflection <= 0.0) {
            return false;
        }

        std::vector<render::PlacedExtent> placed;
        placed.reserve(model.occurrences.size());
        for (const format::OccurrenceRecord& occurrence : model.occurrences) {
            const format::DefinitionInfo& definition = model.definitionInfos[occurrence.definition];
            const std::array<float, 12>& t = occurrence.transform;
            const simd_double3 lo = {definition.boundsMin[0], definition.boundsMin[1],
                                     definition.boundsMin[2]};
            const simd_double3 hi = {definition.boundsMax[0], definition.boundsMax[1],
                                     definition.boundsMax[2]};
            const simd_double3 c = (lo + hi) * 0.5;
            const Vec3 center = {t[0] * c.x + t[3] * c.y + t[6] * c.z + t[9],
                                 t[1] * c.x + t[4] * c.y + t[7] * c.z + t[10],
                                 t[2] * c.x + t[5] * c.y + t[8] * c.z + t[11]};
            placed.push_back({center, simd_length(hi - lo) * 0.5, occurrence.definition});
        }
        const std::vector<u32> needed =
            render::neededLevels(placed, model.definitionInfos.size(), camera, viewport, policy);

        std::vector<u32>& requested = documents[index].requestedLevels;
        requested.resize(needed.size(), 0);
        u32 level = 0;
        std::vector<u32> definitions;
        for (u32 d = 0; d < needed.size(); ++d) {
            const format::DefinitionRecord* geometry = document.geometry(d);
            const u32 have = std::max(requested[d], geometry ? geometry->level : 0);
            if (needed[d] <= have) {
                continue;
            }
            // The finest level needed goes first: it is what the view is closest to.
            if (needed[d] > level) {
                level = needed[d];
                definitions.clear();
            }
            if (needed[d] == level) {
                definitions.push_back(d);
            }
        }
        if (definitions.empty()) {
            return false;
        }
        for (const u32 d : definitions) {
            requested[d] = level;
        }
        startRefinement(index, level, std::move(definitions));
        return true;
    }

    void startRefinement(u32 index, u32 level, std::vector<u32> definitions) {
        refining = true;
        if (traceLaunch) {
            std::fprintf(stderr, "trace: refining %zu definitions to level %u\n",
                         definitions.size(), level);
            geo_trace_memory("refinement started");
        }
        refineCancel = std::make_shared<ipc::ImportCancel>();
        const std::shared_ptr<ipc::ImportCancel> cancel = refineCancel;
        const u64 token = documents[index].token;
        const std::string path = documents[index].path;
        const ipc::ImportPaths paths = importPaths;
        const double start = nowSeconds();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            ImportBridge bridge(this, token);
            const ipc::ImportOutcome outcome =
                ipc::refineFile(path, paths, level, definitions, bridge, cancel.get());
            perform([=, this] {
                if (!documentOf(token)) {
                    return;
                }
                refining = false;
                if (traceLaunch) {
                    geo_trace_memory("refinement finished");
                    std::fprintf(stderr,
                                 "trace: refined %zu definitions to level %u in %.1f ms%s\n",
                                 definitions.size(), level, (nowSeconds() - start) * 1e3,
                                 outcome.ok ? "" : " (failed)");
                }
                if (outcome.ok) {
                    refine(); // there may be more to do at coarser levels
                }
            });
        });
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
                pixelsPerPoint = command->resize.scale;
                look.setScale(pixelsPerPoint);
                applyInsets();
                renderer->resize(command->resize.width, command->resize.height);
                orbit.navigated();
                requestFrame();
                break;
            case Command::Kind::Pointer:
                handlePointer(command->pointer);
                pointerMoved = true;
                break;
            case Command::Kind::OpenFile:
            case Command::Kind::AddFile:
                openFile(command->path, command->kind == Command::Kind::AddFile);
                std::free(command->path);
                break;
            case Command::Kind::Action:
                perform(command->action);
                break;
            case Command::Kind::SetMode:
                interaction.setMode(toMode(command->mode));
                publishedStateVersion = ~u64(0);
                publishState();
                refreshHover();
                break;
            case Command::Kind::SetTool:
                tool = command->tool;
                requestFrame(); // ghosts become clickable, or stop being
                break;
            case Command::Kind::SelectNode:
                if (command->node.document < scene.documentCount() &&
                    scene.document(command->node.document).hasModel() &&
                    command->node.node <
                        scene.document(command->node.document).model().nodes.size()) {
                    interaction.selectNode({command->node.document, command->node.node},
                                           command->node.extend);
                    stateChanged();
                }
                break;
            case Command::Kind::NodeAction:
                if (command->node.document < scene.documentCount() &&
                    scene.document(command->node.document).hasModel() &&
                    command->node.node <
                        scene.document(command->node.document).model().nodes.size()) {
                    nodeAction({command->node.document, command->node.node}, command->node.action);
                }
                break;
            case Command::Kind::HoverNode:
                hoveredNode.reset();
                if (command->node.node != GEO_NO_NODE &&
                    command->node.document < scene.documentCount() &&
                    scene.document(command->node.document).hasModel() &&
                    command->node.node <
                        scene.document(command->node.document).model().nodes.size()) {
                    hoveredNode = model::NodeRef{command->node.document, command->node.node};
                }
                refreshHover();
                break;
            case Command::Kind::SetMouse: {
                std::vector<nav::MouseBinding> bindings;
                const u32 count =
                    std::min<u32>(command->mouse.binding_count, GEO_MAX_MOUSE_BINDINGS);
                for (u32 i = 0; i < count; ++i) {
                    const GeoMouseBinding& b = command->mouse.bindings[i];
                    bindings.push_back({toButton(GeoPointerButton(b.button)), b.modifiers,
                                        nav::Gesture(std::min<u32>(u32(b.gesture), 3))});
                }
                pointer.setBindings(std::move(bindings));
                pointer.setZoom({command->mouse.zoom_toward_pointer, command->mouse.invert_wheel,
                                 std::clamp(double(command->mouse.zoom_speed), 0.1, 10.0)});
                break;
            }
            case Command::Kind::SetSection:
                for (int axis = 0; axis < 3; ++axis) {
                    section.enabled[axis] = command->section.enabled[axis];
                    section.position[axis] = command->section.position[axis];
                    section.flip[axis] = command->section.flip[axis];
                }
                requestFrame();
                break;
            case Command::Kind::SetLook:
                look.set(engine::lookSettings(command->look));
                requestFrame();
                break;
            case Command::Kind::SetInsets:
                insetPoints = command->insets;
                applyInsets();
                break;
            case Command::Kind::Snapshot:
                writeSnapshot(command->path);
                std::free(command->path);
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

    // Draws the current view offscreen and saves it, written to a temporary name first so the file
    // only ever appears whole.
    void writeSnapshot(const std::string& path) {
        renderer->syncState(interaction.state());
        look.apply(*renderer, scene);
        render::ViewState view = makeFrameView();
        render::Overlay overlay;
        if (scene.hasGeometry()) {
            overlay =
                render::makeOverlay(camera, viewport, view.uniforms.viewRotation, pixelsPerPoint,
                                    renderer->glyphAdvance(), cubeHover, overlayLayout());
        }
        const std::vector<u8> pixels = renderer->renderOffscreen(view, &overlay);
        const std::string temporary = path + ".partial";
        if (engine::writePng(temporary.c_str(), pixels, u32(viewport.width),
                             u32(viewport.height))) {
            rename(temporary.c_str(), path.c_str());
        }
    }

    void attachLayer(CAMetalLayer* layer) {
        renderer->attachLayer(layer);
        frames.attach(layer, [this](const gpu::FrameTick& tick) { onFrame(tick); });
        requestFrame();
    }

    // Counts for the app, refreshed when the state changes rather than every frame.
    void publishState() {
        const model::FaceState& state = interaction.state();
        if (state.version() == publishedStateVersion) {
            return;
        }
        publishedStateVersion = state.version();
        u32 selected = 0, hidden = 0;
        for (u32 o = 0; o < state.occurrenceCount(); ++o) {
            selected += state.anyFace(o, kStateSelected) ? 1 : 0;
            hidden += state.anyFace(o, kStateHidden) ? 1 : 0;
        }
        selectedCount.store(selected, std::memory_order_relaxed);
        hiddenCount.store(hidden, std::memory_order_relaxed);

        const auto [occurrences, faces] = interaction.selectionCounts();
        const std::optional<model::NodeRef> focus = interaction.focus();
        std::lock_guard lock(summaryMutex);
        summary = {occurrences, faces, focus ? focus->document : 0,
                   focus ? focus->node : GEO_NO_NODE, u32(interaction.mode())};
    }

    // Finds the shell that see-through fades, from 14 small views of occurrence ids, in two
    // rounds. Runs once, when see-through is first asked for, and again after the scene changes.
    void classifyShells() {
        const std::optional<Aabb> sceneBounds = scene.bounds();
        if (!sceneBounds || !scene.hasGeometry()) {
            return;
        }
        StageTrace trace("shell");
        // Each occurrence's name and world bounds.
        std::vector<render::ShellPart> parts;
        parts.reserve(scene.occurrenceCount());
        for (u32 d = 0; d < scene.documentCount(); ++d) {
            const scene::Document& document = scene.document(d);
            if (!document.hasModel()) {
                continue;
            }
            const format::SegmentView& model = document.model();
            for (const format::OccurrenceRecord& o : model.occurrences) {
                const format::NodeRecord& node = model.nodes[o.node];
                const std::string_view name(model.strings.data() + node.nameOffset,
                                            node.nameLength);
                const format::DefinitionInfo& info = model.definitionInfos[o.definition];
                Aabb box{simd_make_double3(INFINITY, INFINITY, INFINITY),
                         simd_make_double3(-INFINITY, -INFINITY, -INFINITY)};
                for (int corner = 0; corner < 8; ++corner) {
                    const double x = (corner & 1) ? info.boundsMax[0] : info.boundsMin[0];
                    const double y = (corner & 2) ? info.boundsMax[1] : info.boundsMin[1];
                    const double z = (corner & 4) ? info.boundsMax[2] : info.boundsMin[2];
                    const std::array<float, 12>& t = o.transform;
                    const Vec3 p = simd_make_double3(t[0] * x + t[3] * y + t[6] * z + t[9],
                                                     t[1] * x + t[4] * y + t[7] * z + t[10],
                                                     t[2] * x + t[5] * y + t[8] * z + t[11]);
                    box.min = simd_min(box.min, p);
                    box.max = simd_max(box.max, p);
                }
                parts.push_back({name, box});
            }
        }

        const Aabb all = *sceneBounds;
        const double radius = std::max(all.radius(), 1e-9);
        const nav::Viewport square{double(kShellViewSize), double(kShellViewSize)};
        const u32 count = u32(parts.size());
        const auto measure = [&](bool excludeShells) {
            render::ShellCounts counts(count);
            for (const Vec3 direction : render::shellViewDirections()) {
                nav::Camera view;
                const Vec3 up = std::abs(direction.z) > 0.9 ? simd_make_double3(0, 1, 0)
                                                            : simd_make_double3(0, 0, 1);
                view.lookAt(all.center() - direction * (2.0 * radius), all.center(), up);
                view.orthoHalfHeight = radius;
                render::ViewState state = render::makeView(view, square, all);
                state.uniforms.excludeShells = excludeShells ? 1 : 0;
                const gpu::OccurrenceIds ids = renderer->renderOccurrenceIds(state, kShellViewSize);
                counts.accumulate(ids.front, ids.behind);
            }
            return counts;
        };
        renderer->syncState(interaction.state());
        look.apply(*renderer, scene);
        const std::vector<u8> first = render::decideShell(measure(false), parts, 0.3);
        std::vector<u8> shell = first;
        if (std::any_of(first.begin(), first.end(), [](u8 s) { return s != 0; })) {
            // The second layer must hide inner parts decisively, so the mechanism stays solid.
            look.setShells(first);
            look.apply(*renderer, scene);
            const std::vector<u8> second = render::decideShell(measure(true), parts, 0.5);
            usize visibleSolid = 0;
            for (u32 p = 0; p < count; ++p) {
                shell[p] = u8(first[p] | second[p]);
                visibleSolid += shell[p] == 0 ? 1 : 0;
            }
            if (visibleSolid == 0) {
                shell = first;
            }
        }
        look.setShells(shell);
        trace.lap("classify");
    }

    render::OverlayLayout overlayLayout() const {
        render::OverlayLayout layout;
        if (look.settings().style == render::StyleName::Blueprint) {
            layout.ink = render::kOverlayInkOnDark;
        }
        return layout;
    }

    render::ViewState makeFrameView() const {
        render::ViewState view = render::makeView(camera, viewport, bounds);
        view.uniforms.hoverFirst = hover.first;
        view.uniforms.hoverEnd = hover.second;
        view.uniforms.ghostPickable = tool == GeoToolGhost ? 1 : 0;
        view.uniforms.hoverHides = tool == GeoToolHide ? 1 : 0;
        view.uniforms.lineOffset = float(2.0 * scene.coarsestDeflection());
        view.uniforms.aoRadius = float(0.025 * simd_length(bounds.max - bounds.min));
        view.uniforms.aoSamples = 12;
        render::applySection(section, bounds, view.uniforms);
        view.uniforms.seeThrough = seeThrough;
        return view;
    }

    void onFrame(const gpu::FrameTick& tick) {
        // The surface under a still pointer changes as the camera moves, so hover is checked here
        // too.
        updateHover();
        if (seeThrough != seeThroughTarget) {
            const double t =
                std::clamp((nowSeconds() - seeThroughStart) / kSeeThroughSeconds, 0.0, 1.0);
            seeThrough = seeThroughFrom + (seeThroughTarget - seeThroughFrom) * float(nav::ease(t));
            if (t >= 1.0) {
                seeThrough = seeThroughTarget;
            }
            needsFrame = true;
            lastActivity = nowSeconds();
        }
        if (flight.active()) {
            camera = flight.at(nowSeconds());
            orbit.navigated();
            needsFrame = true;
            lastActivity = nowSeconds();
            if (!flight.active()) {
                scheduleRefinement();
            }
        }
        if (needsFrame) {
            needsFrame = false;
            const double start = nowSeconds();
            if (sceneChanged) {
                sceneChanged = false;
                renderer->syncScene(scene);
                if (traceLaunch) {
                    geo_trace_memory("scene synced");
                }
            }
            renderer->syncState(interaction.state());
            look.apply(*renderer, scene);
            publishSwatches();
            const bool heavy = renderer->sceneTriangles() > kHeavyTriangles ||
                               renderer->sceneEdgeSegments() > kHeavyEdgeSegments;
            detailReduced =
                heavy && (flight.active() || nowSeconds() - lastNavigation < kRefineDelaySeconds);
            renderer->setReducedDetail(detailReduced);
            publishState();
            const render::ViewState view = makeFrameView();
            publishedHover.store(hoveredId, std::memory_order_relaxed);
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
            render::Overlay overlay;
            if (scene.hasGeometry()) {
                overlay = render::makeOverlay(camera, viewport, view.uniforms.viewRotation,
                                              pixelsPerPoint, renderer->glyphAdvance(), cubeHover,
                                              overlayLayout());
            }
            const u64 allocationsBefore = engine::allocationCount();
            renderer->render(tick.drawable, view, &overlay);
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
            refineTimer = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 1e9,
                                                          1e9, 0, 0, ^(CFRunLoopTimerRef) {
                                                              @autoreleasepool {
                                                                  if (detailReduced) {
                                                                      requestFrame(); // in full
                                                                  }
                                                                  refine();
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

void geo_engine_add_file(GeoEngine* engine, const char* path) {
    Command command{};
    command.kind = Command::Kind::AddFile;
    command.path = strdup(path);
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

GeoLook geo_default_look(void) {
    return {GeoStyleIllustration, GeoColorMaterial, GeoGhostShell, true, 1.0f, 0.0f};
}

bool geo_style_casts_shadow(GeoStyle style) {
    return render::styleNamed(render::StyleName(std::min<u32>(u32(style), 4))).shadow.on;
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
            engine->publishedHover.load(std::memory_order_relaxed),
            engine->selectedCount.load(std::memory_order_relaxed),
            engine->hiddenCount.load(std::memory_order_relaxed),
            engine->renderer ? engine->renderer->meanGpuSeconds() : 0.0,
            engine->renderer ? engine->renderer->gpuPercentileSeconds(0.99) : 0.0};
}
