// The engine's logic, the same on every platform: what is open, the camera, input, selection and
// visibility, the look, refinement, and what each frame draws. It runs on one thread, which owns
// all its state, and reaches the platform only through its Host and render Backend.
#pragma once

#include "base/types.h"
#include "core/host.h"
#include "core/look_state.h"
#include "engine.h"
#include "math/geometry.h"
#include "model/interaction.h"
#include "nav/camera.h"
#include "nav/flight.h"
#include "nav/orbit_policy.h"
#include "nav/pointer_controller.h"
#include "render/backend.h"
#include "render/overlay.h"
#include "render/shell.h"
#include "render/view.h"
#include "scene/scene.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace geo::engine {

/// What one frame draws, for the platform to hand its renderer with its drawable.
struct Frame {
    render::ViewState view;
    render::Overlay overlay;
    double started = 0.0;   // when the core began preparing it
    double inputTime = 0.0; // when the earliest input it answers arrived, or 0
};

class Core {
public:
    Core(render::Backend& renderer, Host& host);

    // ---- From the app
    // ----------------------------------------------------------------------------

    void resize(u32 width, u32 height, float pixelsPerPoint);
    void setInsets(const nav::Insets& points);
    void pointer(const GeoPointerEvent& event);
    /// After a batch of pointer events: what is under the pointer is checked once, not per event.
    void pointerSettled();
    void perform(GeoAction action);
    void setSelectionMode(GeoSelectionMode mode);
    void setTool(GeoTool tool);
    void selectNode(u32 document, u32 node, bool extend);
    void hoverNode(u32 document, u32 node);
    void nodeAction(u32 document, u32 node, GeoNodeAction action);
    void setLook(const GeoLook& look);
    void setSection(const GeoSection& section);
    void setMouse(const GeoMouseSettings& settings);
    /// Opens a file, replacing what is open, or adding to it.
    void openFile(const std::string& path, bool add);
    void requestFrame();

    // ---- From the platform
    // -----------------------------------------------------------------------

    void addSegment(u64 token, const std::shared_ptr<scene::Segment>& segment);
    void importProgress(u64 token, u32 done, u32 total);
    void importFinished(u64 token, const ImportResult& result);
    void refinementFinished(u64 token, bool ok, u32 definitions, u32 level, double started);
    /// The view has been still long enough: draw in full again, and ask for finer geometry.
    void refinementDue();

    /// Called by the frame clock. Returns what to draw, or nothing when no frame is needed, in
    /// which case it stops the clock once the view has been still a moment.
    std::optional<Frame> frame();

    /// The current view drawn offscreen, as BGRA pixels the size of the viewport.
    std::vector<u8> snapshot();

    nav::Viewport viewport() const { return viewport_; }
    /// The pick id under the pointer as of the last frame, for diagnostics.
    u32 hoveredId() const { return hoveredId_; }

private:
    // One per open file, parallel to the scene's documents. Results carry their document's token,
    // so those of a closed document are dropped.
    struct OpenDocument {
        u64 token = 0;
        std::string path;
        bool importing = true;
        double started = 0.0;
        std::vector<u32> requestedLevels; // the finest level asked for, per definition
        std::string blobPath;             // its cache file, once known
    };

    void emit(GeoEventKind kind, const char* message = "", u32 document = 0, u32 node = GEO_NO_NODE,
              u32 done = 0, u32 total = 0);
    u32 cubeFaceAt(double x, double y) const;
    std::optional<nav::SurfaceHit> pickAt(double x, double y);
    void refreshHover();
    void updateHover();
    void flyTo(nav::StandardView view);
    void flyTo(const nav::Camera& target);
    void flyToFit(const Aabb& box);
    void fitSelection();
    std::optional<Aabb> selectionBounds() const;
    void stateChanged();
    void publishSwatches();
    void visibilityChanged();
    void frameIsolated();
    void resetCamera();
    std::optional<u32> documentOf(u64 token) const;
    void closeAll();
    void publishModel(u32 index);
    void frameScene(bool reset);
    void evictCache();
    void applyInsets();
    void navigated();
    void refine();
    bool refineDocument(u32 index);
    void publishState();
    void classifyShells();
    void requestShellViews(bool excludeShells);
    void shellViewArrived(u64 generation, const render::OccurrenceIds& ids);
    render::OverlayLayout overlayLayout() const;
    render::ViewState frameView() const;
    bool validNode(u32 document, u32 node) const;

    render::Backend& renderer_;
    Host& host_;

    std::vector<OpenDocument> documents_;
    u64 nextToken_ = 1;

    scene::Scene scene_;
    model::Interaction interaction_{scene_};
    Aabb bounds_{simd_make_double3(-1, -1, -1), simd_make_double3(1, 1, 1)};
    bool sceneChanged_ = false;
    bool hasFramed_ = false;
    bool userMovedCamera_ = false;
    // What framing and rotation treat as the model: everything not wholly hidden.
    Aabb visible_{};
    u64 seenVisibility_ = ~u64(0);
    u64 seenSelection_ = ~u64(0);
    std::optional<model::NodeRef> seenFocus_;
    u32 hiddenBodies_ = 0; // bodies with anything hidden
    // The view to return to when an isolate ends, kept from the first of nested isolates.
    std::optional<nav::Camera> cameraBeforeIsolate_;

    // Finer geometry for what the view shows closely. One job runs at a time.
    double lastNavigation_ = -1e9;
    bool detailReduced_ = false; // the last frame left out edges and the shadow
    bool refining_ = false;

    nav::Camera camera_;
    nav::Flight flight_;
    nav::Viewport viewport_;
    float pixelsPerPoint_ = 2.0f;
    nav::Insets insetPoints_; // as the app gives them
    nav::Insets insets_;      // in pixels
    LookState look_;
    render::Section section_;
    // See-through: k moves from where it was toward the target over a short fade.
    float seeThrough_ = 0.0f;
    float seeThroughFrom_ = 0.0f;
    float seeThroughTarget_ = 0.0f;
    double seeThroughStart_ = 0.0;
    u32 cubeHover_ = 0;   // face index plus one, or zero
    u32 cubePressed_ = 0; // the face a press began on, plus one
    nav::OrbitPolicy orbit_;
    nav::PointerController pointer_;
    render::DepthDecoder depthDecoder_;

    float cursorX_ = -1, cursorY_ = -1;
    bool cursorInside_ = false;
    u32 hoveredId_ = 0;
    std::pair<u32, u32> hover_{0, 0};
    std::optional<model::NodeRef> hoveredNode_;
    std::optional<std::pair<model::NodeRef, u32>> hoveredPart_;
    GeoTool tool_ = GeoToolSelect;
    u64 publishedStateVersion_ = ~u64(0);

    // Classifying the shell: two rounds of views, whose ids may arrive frames later. A newer
    // classification, or closing everything, makes the one in flight stale.
    struct ShellClassification {
        u64 generation = 0;
        bool secondRound = false;
        u32 arrived = 0;
        std::vector<render::ShellPart> parts;
        render::ShellCounts counts{0};
        std::vector<u8> first;
        double started = 0.0;
    };
    std::optional<ShellClassification> classifying_;
    u64 classifyGeneration_ = 0;
    // Held weakly by callbacks the renderer makes later, so none reaches a core that is gone.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    bool needsFrame_ = false;
    double lastActivity_ = 0.0;
    double pendingInputTime_ = 0.0;
    const bool traceLaunch_;
};

} // namespace geo::engine
