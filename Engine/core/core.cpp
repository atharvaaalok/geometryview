#include "core/core.h"

#include "base/trace.h"
#include "model/visibility.h"
#include "render/refinement.h"
#include "render/shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geo::engine {
namespace {

// ProMotion drops its refresh rate when presents stop, so frames keep flowing briefly after the
// last input, which keeps the next interaction at full rate.
constexpr double kLingerSeconds = 0.25;
constexpr double kFitMargin = 1.12;
constexpr double kDefaultAzimuth = 35.0 * M_PI / 180.0;
constexpr double kDefaultElevation = 25.0 * M_PI / 180.0;
// Refinement waits for the view to settle, so a drag never starts work it would then outrun.
constexpr double kRefineDelaySeconds = 0.3;
// Camera flights, as to a standard view, take this long unless motion is reduced.
constexpr double kFlightSeconds = 0.55;
// See-through fades in and out over this long, as in Colorfigures.
constexpr double kSeeThroughSeconds = 0.7;
// The square views that classify the shell, in pixels.
constexpr u32 kShellViewSize = 160;

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

// The last component of a path, for naming a file to the app.
std::string fileName(const std::string& path) {
    const usize slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

Core::Core(render::Backend& renderer, Host& host)
    : renderer_(renderer), host_(host),
      pointer_(camera_, viewport_, orbit_, [this](double x, double y) { return pickAt(x, y); }),
      traceLaunch_(traceEnabled()) {}

void Core::emit(GeoEventKind kind, const char* message, u32 document, u32 node, u32 done,
                u32 total) {
    if (traceLaunch_ && kind != GeoEventProgress) {
        std::fprintf(stderr, "trace: event %d: %s\n", int(kind), message);
    }
    const GeoEvent event{kind, document, node, done, total, message};
    host_.emit(event);
}

void Core::requestFrame() {
    needsFrame_ = true;
    lastActivity_ = host_.now();
    host_.runFrames(true);
}

// The view cube's face under a pixel, plus one, or zero.
u32 Core::cubeFaceAt(double x, double y) const {
    const std::optional<render::PixelHit> pixel =
        renderer_.readPixel(int(x), int(y), depthDecoder_);
    return pixel && pixel->id >= kCubeIdBase ? pixel->id - kCubeIdBase + 1 : 0;
}

// The model's surface under a pixel. The view cube is not a surface: it is no pivot to turn about,
// and clicking it is handled on its own.
std::optional<nav::SurfaceHit> Core::pickAt(double x, double y) {
    const std::optional<render::PixelHit> pixel =
        renderer_.readPixel(int(x), int(y), depthDecoder_);
    if (!pixel || pixel->id >= kCubeIdBase) {
        return std::nullopt;
    }
    const double ndcX = 2.0 * x / viewport_.width - 1.0;
    const double ndcY = 1.0 - 2.0 * y / viewport_.height;
    return nav::SurfaceHit{pixel->id, camera_.unproject(ndcX, ndcY, pixel->viewDepth, viewport_),
                           pixel->viewDepth};
}

// What hover lights up: a tree row the pointer is over, or else the surface under the pointer.
void Core::refreshHover() {
    const std::pair<u32, u32> range =
        hoveredNode_ ? interaction_.nodeRange(*hoveredNode_) : interaction_.hoverRange(hoveredId_);
    if (range != hover_) {
        hover_ = range;
        requestFrame();
    }
    // The app labels what is under the pointer, so it hears when that changes, not per frame.
    std::optional<std::pair<model::NodeRef, u32>> part;
    if (!hoveredNode_) {
        part = interaction_.partAt(hoveredId_);
    }
    if (part != hoveredPart_) {
        hoveredPart_ = part;
        if (part) {
            emit(GeoEventHoverChanged, "", part->first.document, part->first.node, part->second);
        } else {
            emit(GeoEventHoverChanged, "", 0, GEO_NO_NODE);
        }
    }
}

// Reading the ID buffer costs nanoseconds and no GPU work. A frame is drawn only if the surface
// under the pointer actually changed.
void Core::updateHover() {
    if (pointer_.isPressed()) {
        return;
    }
    u32 id = 0;
    u32 cube = 0;
    if (cursorInside_) {
        cube = cubeFaceAt(cursorX_, cursorY_);
        if (const std::optional<nav::SurfaceHit> hit = pickAt(cursorX_, cursorY_)) {
            id = hit->id;
        }
    }
    if (cube != cubeHover_) {
        cubeHover_ = cube;
        requestFrame();
    }
    if (id != hoveredId_) {
        hoveredId_ = id;
        refreshHover();
    }
}

// Flies the camera to a standard view of everything shown.
void Core::flyTo(nav::StandardView view) {
    if (!scene_.hasGeometry()) {
        return;
    }
    flyTo(nav::standardView(camera_, view, visible_, viewport_, kFitMargin, insets_));
}

void Core::flyTo(const nav::Camera& target) {
    flight_.start(camera_, target, host_.now(), host_.reduceMotion() ? 0.0 : kFlightSeconds);
    requestFrame();
}

void Core::flyToFit(const Aabb& box) {
    nav::Camera target = camera_;
    nav::fit(target, box, viewport_, kFitMargin, insets_);
    flyTo(target);
}

// Flies to frame what is selected, or everything if nothing is.
void Core::fitSelection() {
    const std::optional<Aabb> selected = selectionBounds();
    if (!selected) {
        perform(GeoActionFit);
        return;
    }
    flyToFit(*selected);
}

// Bounds of the occurrences with anything selected, from their definitions' bounds.
std::optional<Aabb> Core::selectionBounds() const {
    const model::FaceState& state = interaction_.state();
    return model::boundsWhere(
        scene_, [&](u32 occurrence) { return state.anyFace(occurrence, kStateSelected); });
}

// After anything changes what is hidden, ghosted or selected.
void Core::stateChanged() {
    refreshHover();
    requestFrame();
    publishState();
    if (interaction_.state().visibilityVersion() != seenVisibility_) {
        visibilityChanged();
    }
    // Only a change of selection, so hiding from the tree does not scroll it to the selection.
    const std::optional<model::NodeRef> focus = interaction_.focus();
    if (interaction_.state().selectionVersion() == seenSelection_ && focus == seenFocus_) {
        return;
    }
    seenSelection_ = interaction_.state().selectionVersion();
    seenFocus_ = focus;
    if (focus) {
        emit(GeoEventSelectionChanged, "", focus->document, focus->node);
    } else {
        emit(GeoEventSelectionChanged, "", 0, GEO_NO_NODE);
    }
}

// Hands the app each occurrence's swatch colour when the looks change: rarely, never per frame.
void Core::publishSwatches() {
    if (!look_.swatchesChanged()) {
        return;
    }
    std::vector<u32> swatches = look_.takeSwatches();
    std::vector<u32> bases(scene_.documentCount());
    for (u32 d = 0; d < bases.size(); ++d) {
        bases[d] = scene_.occurrenceBase(d);
    }
    host_.publishSwatches(std::move(swatches), std::move(bases));
    emit(GeoEventLooksChanged);
}

// After what is shown changes, by hiding, ghosting or opening files: rotation and framing follow
// what is shown, as though it were all that had been opened, and the end of an isolate returns the
// view it began from.
void Core::visibilityChanged() {
    const model::FaceState& state = interaction_.state();
    seenVisibility_ = state.visibilityVersion();
    visible_ = model::visibleBounds(scene_, state).value_or(bounds_);
    orbit_.setVisibleBounds(visible_);
    u32 hidden = 0, ghosted = 0;
    for (u32 o = 0; o < state.occurrenceCount(); ++o) {
        hidden += state.anyFace(o, kStateHidden) ? 1 : 0;
        ghosted += state.anyFace(o, kStateGhost) ? 1 : 0;
    }
    hiddenBodies_ = hidden;
    if (cameraBeforeIsolate_ && hidden == 0) {
        flyTo(*cameraBeforeIsolate_);
        cameraBeforeIsolate_.reset();
    }
    std::vector<std::vector<model::NodeVisibility>> states(scene_.documentCount());
    for (u32 d = 0; d < states.size(); ++d) {
        states[d] = model::nodeStates(scene_, state, d);
    }
    host_.publishNodeStates(std::move(states));
    emit(GeoEventVisibilityChanged, "", 0, GEO_NO_NODE, hidden, ghosted);
}

bool Core::validNode(u32 document, u32 node) const {
    return document < scene_.documentCount() && scene_.document(document).hasModel() &&
           node < scene_.document(document).model().nodes.size();
}

// A tree row's button.
void Core::nodeAction(u32 document, u32 node, GeoNodeAction action) {
    if (!validNode(document, node)) {
        return;
    }
    const model::NodeRef ref{document, node};
    switch (action) {
    case GeoNodeHide:
        interaction_.setNode(ref, kStateHidden, true);
        break;
    case GeoNodeShow:
        interaction_.setNode(ref, kStateHidden, false);
        break;
    case GeoNodeGhost:
        interaction_.setNode(ref, kStateGhost, true);
        break;
    case GeoNodeUnghost:
        interaction_.setNode(ref, kStateGhost, false);
        break;
    case GeoNodeIsolate: {
        const u64 before = interaction_.state().visibilityVersion();
        interaction_.isolateNode(ref);
        stateChanged();
        if (interaction_.state().visibilityVersion() != before) {
            frameIsolated();
        }
        return;
    }
    }
    stateChanged();
}

// Isolating frames what is kept, as if it alone had been opened, and remembers the view to return
// to when everything is shown again.
void Core::frameIsolated() {
    if (hiddenBodies_ == 0) {
        return; // everything was kept, so nothing was isolated
    }
    if (!cameraBeforeIsolate_) {
        cameraBeforeIsolate_ = flight_.active() ? flight_.destination() : camera_;
    }
    flyToFit(visible_);
}

void Core::pointer(const GeoPointerEvent& event) {
    lastActivity_ = host_.now();
    if (event.kind == GeoPointerLeave) {
        cursorInside_ = false;
        updateHover();
        return;
    }
    if (event.kind == GeoPointerCancel) {
        pointer_.cancel();
        cubePressed_ = 0;
        updateHover();
        return;
    }
    cursorX_ = event.x;
    cursorY_ = event.y;
    cursorInside_ = true;

    nav::PointerResponse response;
    switch (event.kind) {
    case GeoPointerMove:
        response = pointer_.move(event.x, event.y);
        break;
    case GeoPointerDown:
        host_.runFrames(true);
        flight_.cancel();
        cubePressed_ = event.button == GeoButtonLeft ? cubeFaceAt(event.x, event.y) : 0;
        response = pointer_.press(toButton(event.button), event.modifiers, event.x, event.y);
        break;
    case GeoPointerUp:
        response = pointer_.release(toButton(event.button), event.x, event.y);
        break;
    case GeoPointerScroll:
    case GeoPointerMagnify:
        response = pointer_.scroll(event.delta, event.x, event.y, event.timestamp);
        break;
    case GeoPointerLeave:
    case GeoPointerCancel:
        break;
    }

    if (response.cameraMoved) {
        userMovedCamera_ = true;
        navigated();
        pendingInputTime_ = pendingInputTime_ == 0.0 ? event.timestamp : pendingInputTime_;
        requestFrame();
    }
    if (response.clicked && cubePressed_ != 0) {
        flyTo(nav::StandardView(cubePressed_ - 1));
    } else if (response.clicked) {
        const bool extend = (event.modifiers & (nav::kModifierShift | nav::kModifierCommand)) != 0;
        interaction_.click(response.clickedId.value_or(0), toTool(tool_), extend);
        stateChanged();
    }
    if (event.kind == GeoPointerUp) {
        cubePressed_ = 0;
    }
}

void Core::pointerSettled() {
    updateHover();
}

void Core::resetCamera() {
    const Vec3 direction = simd_make_double3(
        std::cos(kDefaultElevation) * std::sin(kDefaultAzimuth),
        -std::cos(kDefaultElevation) * std::cos(kDefaultAzimuth), std::sin(kDefaultElevation));
    camera_.lookAt(bounds_.center() + direction, bounds_.center(),
                   simd_make_double3(0.0, 0.0, 1.0));
    nav::fit(camera_, bounds_, viewport_, kFitMargin, insets_);
}

void Core::perform(GeoAction action) {
    lastActivity_ = host_.now();
    switch (action) {
    case GeoActionFit:
        if (scene_.hasGeometry()) {
            nav::fit(camera_, visible_, viewport_, kFitMargin, insets_);
            orbit_.navigated();
            navigated();
        }
        break;
    case GeoActionToggleProjection:
        camera_.perspective = !camera_.perspective;
        if (scene_.hasGeometry()) {
            nav::fit(camera_, visible_, viewport_, kFitMargin, insets_);
        }
        orbit_.navigated();
        navigated();
        break;
    case GeoActionClearSelection:
        interaction_.clearSelection();
        break;
    case GeoActionShowAll:
        interaction_.showAll();
        break;
    case GeoActionUnghostAll:
        interaction_.unghostAll();
        break;
    case GeoActionIsolate: {
        const u64 before = interaction_.state().visibilityVersion();
        interaction_.isolate();
        stateChanged();
        if (interaction_.state().visibilityVersion() != before) {
            frameIsolated();
        }
        return;
    }
    case GeoActionHideSelection:
        interaction_.hideSelection();
        break;
    case GeoActionGhostSelection:
        interaction_.ghostSelection();
        break;
    case GeoActionUndo:
        interaction_.undo();
        break;
    case GeoActionRedo:
        interaction_.redo();
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
        if (seeThroughTarget_ == 0.0f && !look_.hasShells()) {
            classifyShells();
        }
        seeThroughFrom_ = seeThrough_;
        seeThroughTarget_ = seeThroughTarget_ > 0.0f ? 0.0f : 1.0f;
        seeThroughStart_ = host_.now();
        if (host_.reduceMotion()) {
            seeThrough_ = seeThroughTarget_;
        }
        requestFrame();
        return;
    }
    stateChanged();
}

void Core::setSelectionMode(GeoSelectionMode mode) {
    interaction_.setMode(toMode(mode));
    publishedStateVersion_ = ~u64(0);
    publishState();
    refreshHover();
}

void Core::setTool(GeoTool tool) {
    tool_ = tool;
    requestFrame(); // ghosts become clickable, or stop being
}

void Core::selectNode(u32 document, u32 node, bool extend) {
    if (validNode(document, node)) {
        interaction_.selectNode({document, node}, extend);
        stateChanged();
    }
}

void Core::hoverNode(u32 document, u32 node) {
    hoveredNode_.reset();
    if (node != GEO_NO_NODE && validNode(document, node)) {
        hoveredNode_ = model::NodeRef{document, node};
    }
    refreshHover();
}

void Core::setLook(const GeoLook& look) {
    look_.set(lookSettings(look));
    requestFrame();
}

void Core::setSection(const GeoSection& section) {
    for (int axis = 0; axis < 3; ++axis) {
        section_.enabled[axis] = section.enabled[axis];
        section_.position[axis] = section.position[axis];
        section_.flip[axis] = section.flip[axis];
    }
    requestFrame();
}

void Core::setMouse(const GeoMouseSettings& settings) {
    std::vector<nav::MouseBinding> bindings;
    const u32 count = std::min<u32>(settings.binding_count, GEO_MAX_MOUSE_BINDINGS);
    for (u32 i = 0; i < count; ++i) {
        const GeoMouseBinding& b = settings.bindings[i];
        bindings.push_back({toButton(GeoPointerButton(b.button)), b.modifiers,
                            nav::Gesture(std::min<u32>(u32(b.gesture), 3))});
    }
    pointer_.setBindings(std::move(bindings));
    pointer_.setZoom({settings.zoom_toward_pointer, settings.invert_wheel,
                      std::clamp(double(settings.zoom_speed), 0.1, 10.0)});
}

void Core::setViewDirection(double azimuthDegrees, double elevationDegrees, bool perspective) {
    const double azimuth = azimuthDegrees * M_PI / 180.0;
    const double elevation = elevationDegrees * M_PI / 180.0;
    const Vec3 direction =
        simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                          -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    const Aabb box = visible_.min.x <= visible_.max.x ? visible_ : bounds_;
    flight_.cancel();
    camera_.perspective = perspective;
    camera_.lookAt(box.center() + direction, box.center(), simd_make_double3(0.0, 0.0, 1.0));
    nav::fit(camera_, box, viewport_, kFitMargin, insets_);
    userMovedCamera_ = true;
    navigated();
    requestFrame();
}

u64 Core::sceneFaces() const {
    u64 faces = 0;
    for (u32 d = 0; d < scene_.documentCount(); ++d) {
        faces += scene_.document(d).idCount();
    }
    return faces;
}

// Every placed triangle, counted from the scene rather than from the renderer, which knows only
// once a frame has synced it.
u64 Core::sceneTriangles() const {
    u64 triangles = 0;
    for (u32 d = 0; d < scene_.documentCount(); ++d) {
        const scene::Document& document = scene_.document(d);
        if (!document.hasModel()) {
            continue;
        }
        for (const format::OccurrenceRecord& occurrence : document.model().occurrences) {
            if (const format::DefinitionRecord* geometry =
                    document.geometry(occurrence.definition)) {
                triangles += geometry->triangleCount;
            }
        }
    }
    return triangles;
}

void Core::setDetailBudget(u64 triangles, u64 edgeSegments) {
    detailTriangles_ = triangles;
    detailEdgeSegments_ = edgeSegments;
}

void Core::resize(u32 width, u32 height, float pixelsPerPoint) {
    viewport_ = {double(width), double(height)};
    pointer_.setPixelsPerPoint(pixelsPerPoint);
    pixelsPerPoint_ = pixelsPerPoint;
    look_.setScale(pixelsPerPoint);
    applyInsets();
    renderer_.resize(width, height);
    orbit_.navigated();
    requestFrame();
}

void Core::setInsets(const nav::Insets& points) {
    insetPoints_ = points;
    applyInsets();
}

void Core::applyInsets() {
    insets_ = {insetPoints_.left * pixelsPerPoint_, insetPoints_.top * pixelsPerPoint_,
               insetPoints_.right * pixelsPerPoint_, insetPoints_.bottom * pixelsPerPoint_};
}

std::optional<u32> Core::documentOf(u64 token) const {
    for (u32 d = 0; d < documents_.size(); ++d) {
        if (documents_[d].token == token) {
            return d;
        }
    }
    return std::nullopt;
}

void Core::closeAll() {
    for (const OpenDocument& document : documents_) {
        host_.cancelImport(document.token);
    }
    host_.cancelRefinement();
    refining_ = false;
    classifying_.reset();
    documents_.clear();
    scene_.clear();
    interaction_.reset();
    look_.sceneChanged();
    host_.clearModels();
    hasFramed_ = false;
    userMovedCamera_ = false;
    cameraBeforeIsolate_.reset();
    seenVisibility_ = ~u64(0);
    seenSelection_ = ~u64(0);
    seenFocus_.reset();
    host_.publishNodeStates({});
    hoveredId_ = 0;
    hoveredNode_.reset();
    hover_ = {0, 0};
}

void Core::openFile(const std::string& path, bool add) {
    if (!add) {
        closeAll();
    }
    const u32 index = scene_.addDocument();
    OpenDocument& document = documents_.emplace_back();
    document.token = nextToken_++;
    document.path = path;
    document.started = host_.now();
    emit(GeoEventLoading, fileName(path).c_str(), index);
    sceneChanged_ = true;
    requestFrame();
    // The import can take seconds, and the core must keep drawing meanwhile.
    host_.startImport(document.token, path);
}

void Core::addSegment(u64 token, const std::shared_ptr<scene::Segment>& segment) {
    const std::optional<u32> index = documentOf(token);
    if (!index) {
        return;
    }
    scene::Document& document = scene_.document(*index);
    const bool hadModel = document.hasModel();
    std::string error;
    if (!document.add(segment, error)) {
        emit(GeoEventFailed, error.c_str(), *index);
        return;
    }
    sceneChanged_ = true;
    if (!hadModel && document.hasModel()) {
        interaction_.sceneChanged();
        look_.sceneChanged();
        publishModel(*index);
    }
    if (!hasFramed_) {
        frameScene(true);
    } else if (!hadModel && document.hasModel()) {
        visibilityChanged(); // a file added to what is shown
    }
    requestFrame();
}

void Core::publishModel(u32 index) {
    for (const std::shared_ptr<scene::Segment>& segment : scene_.document(index).segments()) {
        if (segment->view().hasModel()) {
            host_.publishModel(index, segment);
        }
    }
    emit(GeoEventModelReady, "", index);
}

void Core::importProgress(u64 token, u32 done, u32 total) {
    if (const std::optional<u32> index = documentOf(token)) {
        emit(GeoEventProgress, "", *index, GEO_NO_NODE, done, total);
    }
}

// Takes new scene bounds, and frames them unless the user has already moved the camera.
void Core::frameScene(bool reset) {
    const std::optional<Aabb> current = scene_.bounds();
    if (!current) {
        return;
    }
    bounds_ = *current;
    if (reset) {
        resetCamera();
        hasFramed_ = true;
    } else if (!userMovedCamera_) {
        nav::fit(camera_, bounds_, viewport_, kFitMargin, insets_);
    }
    visibilityChanged();
}

// Keeps the cache within its budget, sparing what is open.
void Core::evictCache() {
    std::vector<std::string> keep;
    for (const OpenDocument& open : documents_) {
        if (!open.blobPath.empty()) {
            keep.push_back(open.blobPath);
        }
    }
    host_.evictCache(std::move(keep));
}

void Core::importFinished(u64 token, const ImportResult& result) {
    const std::optional<u32> index = documentOf(token);
    if (!index) {
        return; // closed, or replaced by another file
    }
    OpenDocument& document = documents_[*index];
    document.importing = false;
    navigated();
    if (!result.ok) {
        emit(GeoEventFailed, result.message.c_str(), *index);
        return;
    }
    document.blobPath = result.blobPath;
    if (!result.fromCache) {
        evictCache();
    }
    frameScene(false); // estimated bounds become exact now that all geometry is here
    if (seeThroughTarget_ > 0.0f) {
        classifyShells(); // the shell is found again for the scene as it is now
    }
    requestFrame();
    if (traceLaunch_) {
        std::fprintf(stderr, "trace: scene ready %.1f ms after opening\n",
                     (host_.now() - document.started) * 1e3);
    }
    if (std::none_of(documents_.begin(), documents_.end(),
                     [](const OpenDocument& open) { return open.importing; })) {
        emit(GeoEventSceneReady, "", *index);
    }
}

// The view moved: refinement and full detail wait until it has been still a moment.
void Core::navigated() {
    lastNavigation_ = host_.now();
    host_.scheduleRefinement(kRefineDelaySeconds);
}

void Core::refinementDue() {
    if (detailReduced_) {
        requestFrame(); // in full
    }
    refine();
}

// Asks for finer geometry where the settled view shows more than half a pixel of chord error.
void Core::refine() {
    if (refining_) {
        return;
    }
    for (u32 index = 0; index < documents_.size(); ++index) {
        if (!documents_[index].importing && refineDocument(index)) {
            return;
        }
    }
}

bool Core::refineDocument(u32 index) {
    const scene::Document& document = scene_.document(index);
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
        render::neededLevels(placed, model.definitionInfos.size(), camera_, viewport_, policy);

    std::vector<u32>& requested = documents_[index].requestedLevels;
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
    refining_ = true;
    if (traceLaunch_) {
        std::fprintf(stderr, "trace: refining %zu definitions to level %u\n", definitions.size(),
                     level);
        host_.traceMemory("refinement started");
    }
    host_.startRefinement(documents_[index].token, documents_[index].path, level,
                          std::move(definitions));
    return true;
}

void Core::refinementFinished(u64 token, bool ok, u32 definitions, u32 level, double started) {
    if (!documentOf(token)) {
        return;
    }
    refining_ = false;
    if (traceLaunch_) {
        host_.traceMemory("refinement finished");
        std::fprintf(stderr, "trace: refined %u definitions to level %u in %.1f ms%s\n",
                     definitions, level, (host_.now() - started) * 1e3, ok ? "" : " (failed)");
    }
    if (ok) {
        refine(); // there may be more to do at coarser levels
    }
}

// Counts for the app, refreshed when the state changes rather than every frame.
void Core::publishState() {
    const model::FaceState& state = interaction_.state();
    if (state.version() == publishedStateVersion_) {
        return;
    }
    publishedStateVersion_ = state.version();
    Summary summary;
    for (u32 o = 0; o < state.occurrenceCount(); ++o) {
        summary.selectedOccurrences += state.anyFace(o, kStateSelected) ? 1 : 0;
        summary.hiddenOccurrences += state.anyFace(o, kStateHidden) ? 1 : 0;
    }
    const auto [occurrences, faces] = interaction_.selectionCounts();
    const std::optional<model::NodeRef> focus = interaction_.focus();
    summary.selection = {occurrences, faces, focus ? focus->document : 0,
                         focus ? focus->node : GEO_NO_NODE, u32(interaction_.mode())};
    host_.publishSummary(summary);
}

// Finds the shell that see-through fades, from 14 small views of occurrence ids, in two rounds.
// Runs once, when see-through is first asked for, and again after the scene changes. The views are
// read back at once on the Mac, and frames later in the browser.
void Core::classifyShells() {
    const std::optional<Aabb> sceneBounds = scene_.bounds();
    if (!sceneBounds || !scene_.hasGeometry()) {
        return;
    }
    ShellClassification& c = classifying_.emplace();
    c.generation = ++classifyGeneration_;
    c.started = host_.now();
    // Each occurrence's name and world bounds.
    c.parts.reserve(scene_.occurrenceCount());
    for (u32 d = 0; d < scene_.documentCount(); ++d) {
        const scene::Document& document = scene_.document(d);
        if (!document.hasModel()) {
            continue;
        }
        const format::SegmentView& model = document.model();
        for (const format::OccurrenceRecord& o : model.occurrences) {
            const format::NodeRecord& node = model.nodes[o.node];
            const std::string_view name(model.strings.data() + node.nameOffset, node.nameLength);
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
            c.parts.push_back({name, box});
        }
    }
    renderer_.syncState(interaction_.state());
    look_.apply(renderer_, scene_);
    requestShellViews(false);
}

void Core::requestShellViews(bool excludeShells) {
    ShellClassification& c = *classifying_;
    c.secondRound = excludeShells;
    c.arrived = 0;
    c.counts = render::ShellCounts(u32(c.parts.size()));
    const Aabb all = *scene_.bounds();
    const double radius = std::max(all.radius(), 1e-9);
    const nav::Viewport square{double(kShellViewSize), double(kShellViewSize)};
    const u64 generation = c.generation;
    for (const Vec3 direction : render::shellViewDirections()) {
        nav::Camera view;
        const Vec3 up =
            std::abs(direction.z) > 0.9 ? simd_make_double3(0, 1, 0) : simd_make_double3(0, 0, 1);
        view.lookAt(all.center() - direction * (2.0 * radius), all.center(), up);
        view.orthoHalfHeight = radius;
        render::ViewState state = render::makeView(view, square, all);
        state.uniforms.excludeShells = excludeShells ? 1 : 0;
        renderer_.requestOccurrenceIds(
            state, kShellViewSize,
            [this, alive = std::weak_ptr<int>(alive_), generation](render::OccurrenceIds ids) {
                if (alive.lock()) {
                    shellViewArrived(generation, ids);
                }
            });
        // On the Mac each view arrives within the call, and the last may have ended this round.
        if (!classifying_ || classifying_->generation != generation ||
            classifying_->secondRound != excludeShells) {
            return;
        }
    }
}

void Core::shellViewArrived(u64 generation, const render::OccurrenceIds& ids) {
    if (!classifying_ || classifying_->generation != generation) {
        return; // a newer classification, or the scene it measured is gone
    }
    ShellClassification& c = *classifying_;
    c.counts.accumulate(ids.front, ids.behind);
    if (++c.arrived < render::shellViewDirections().size()) {
        return;
    }
    if (!c.secondRound) {
        c.first = render::decideShell(c.counts, c.parts, 0.3);
        if (std::any_of(c.first.begin(), c.first.end(), [](u8 s) { return s != 0; })) {
            // The second layer must hide inner parts decisively, so the mechanism stays solid.
            look_.setShells(c.first);
            look_.apply(renderer_, scene_);
            requestShellViews(true);
            return;
        }
        look_.setShells(c.first);
    } else {
        const std::vector<u8> second = render::decideShell(c.counts, c.parts, 0.5);
        std::vector<u8> shell = c.first;
        usize visibleSolid = 0;
        for (usize p = 0; p < shell.size(); ++p) {
            shell[p] = u8(c.first[p] | second[p]);
            visibleSolid += shell[p] == 0 ? 1 : 0;
        }
        look_.setShells(visibleSolid == 0 ? c.first : shell);
    }
    if (traceLaunch_) {
        std::fprintf(stderr, "trace: shell classified in %.1f ms\n",
                     (host_.now() - c.started) * 1e3);
    }
    classifying_.reset();
    requestFrame();
}

render::OverlayLayout Core::overlayLayout() const {
    render::OverlayLayout layout;
    if (look_.settings().style == render::StyleName::Blueprint) {
        layout.ink = render::kOverlayInkOnDark;
    }
    return layout;
}

render::ViewState Core::frameView() const {
    render::ViewState view = render::makeView(camera_, viewport_, bounds_);
    view.uniforms.hoverFirst = hover_.first;
    view.uniforms.hoverEnd = hover_.second;
    view.uniforms.ghostPickable = tool_ == GeoToolGhost ? 1 : 0;
    view.uniforms.hoverHides = tool_ == GeoToolHide ? 1 : 0;
    view.uniforms.lineOffset = float(2.0 * scene_.coarsestDeflection());
    view.uniforms.aoRadius = float(0.025 * simd_length(bounds_.max - bounds_.min));
    view.uniforms.aoSamples = 12;
    render::applySection(section_, bounds_, view.uniforms);
    view.uniforms.seeThrough = seeThrough_;
    return view;
}

std::optional<Frame> Core::frame() {
    // The surface under a still pointer changes as the camera moves, so hover is checked here too.
    updateHover();
    const double now = host_.now();
    if (seeThrough_ != seeThroughTarget_) {
        const double t = std::clamp((now - seeThroughStart_) / kSeeThroughSeconds, 0.0, 1.0);
        seeThrough_ = seeThroughFrom_ + (seeThroughTarget_ - seeThroughFrom_) * float(nav::ease(t));
        if (t >= 1.0) {
            seeThrough_ = seeThroughTarget_;
        }
        needsFrame_ = true;
        lastActivity_ = now;
    }
    if (flight_.active()) {
        camera_ = flight_.at(now);
        orbit_.navigated();
        needsFrame_ = true;
        lastActivity_ = now;
        if (!flight_.active()) {
            navigated();
        }
    }
    if (!needsFrame_) {
        if (now - lastActivity_ > kLingerSeconds) {
            host_.runFrames(false);
        }
        return std::nullopt;
    }
    needsFrame_ = false;
    if (sceneChanged_) {
        sceneChanged_ = false;
        renderer_.syncScene(scene_);
        if (traceLaunch_) {
            host_.traceMemory("scene synced");
        }
    }
    renderer_.syncState(interaction_.state());
    look_.apply(renderer_, scene_);
    publishSwatches();
    const bool heavy = renderer_.sceneTriangles() > detailTriangles_ ||
                       renderer_.sceneEdgeSegments() > detailEdgeSegments_;
    detailReduced_ = heavy && (flight_.active() || now - lastNavigation_ < kRefineDelaySeconds);
    renderer_.setReducedDetail(detailReduced_);
    publishState();

    Frame frame;
    frame.started = now;
    frame.view = frameView();
    frame.inputTime = pendingInputTime_;
    pendingInputTime_ = 0.0;
    depthDecoder_ = frame.view.depth;
    if (scene_.hasGeometry()) {
        frame.overlay = render::makeOverlay(camera_, viewport_, frame.view.uniforms.viewRotation,
                                            pixelsPerPoint_, renderer_.glyphAdvance(), cubeHover_,
                                            overlayLayout());
    }
    return frame;
}

std::vector<u8> Core::snapshot() {
    renderer_.syncState(interaction_.state());
    look_.apply(renderer_, scene_);
    const render::ViewState view = frameView();
    render::Overlay overlay;
    if (scene_.hasGeometry()) {
        overlay =
            render::makeOverlay(camera_, viewport_, view.uniforms.viewRotation, pixelsPerPoint_,
                                renderer_.glyphAdvance(), cubeHover_, overlayLayout());
    }
    return renderer_.renderOffscreen(view, &overlay);
}

} // namespace geo::engine
