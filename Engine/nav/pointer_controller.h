// Turns pointer input into camera motion and picks.
#pragma once

#include "nav/camera.h"
#include "nav/orbit_policy.h"

#include "base/types.h"

#include <functional>
#include <optional>
#include <vector>

namespace geo::nav {

enum class Button : u8 { None, Left, Right, Middle };

enum Modifier : u32 {
    kModifierShift = 1 << 0,
    kModifierControl = 1 << 1,
    kModifierOption = 1 << 2,
    kModifierCommand = 1 << 3,
    kModifierSpace = 1 << 4,
};

enum class Gesture : u8 { None, Rotate, Pan, Dolly };

/// One row of the mouse mapping. Nothing about the mouse is hard-coded: the settings UI edits this
/// table, and the most specific matching row wins.
struct MouseBinding {
    Button button;
    u32 modifiers;
    Gesture gesture;
};

std::vector<MouseBinding> defaultMouseBindings();

/// What a press hits, if anything.
struct SurfaceHit {
    u32 id;
    Vec3 point;
    double viewDepth;
};

/// Looks up the surface at a viewport pixel.
using PixelPicker = std::function<std::optional<SurfaceHit>(double x, double y)>;

struct PointerResponse {
    bool cameraMoved = false;
    bool clicked = false; // a press and release without dragging
    std::optional<u32> clickedId;
};

class PointerController {
public:
    PointerController(Camera& camera, Viewport& viewport, OrbitPolicy& orbit, PixelPicker picker);

    /// `pixelsPerPoint` converts the point-based thresholds to pixels.
    void setPixelsPerPoint(double scale) { pixelsPerPoint_ = scale; }
    void setBindings(std::vector<MouseBinding> bindings) { bindings_ = std::move(bindings); }
    const std::vector<MouseBinding>& bindings() const { return bindings_; }

    /// How the wheel zooms: toward the pointer or the centre of the view, which way, how fast.
    struct ZoomSettings {
        bool towardPointer = true;
        bool invert = false;
        double speed = 1.0;
    };
    void setZoom(ZoomSettings zoom) { zoom_ = zoom; }

    bool isDragging() const { return active_ != Gesture::None; }
    bool isPressed() const { return pressed_ != Button::None; }

    PointerResponse press(Button button, u32 modifiers, double x, double y);
    PointerResponse move(double x, double y);
    PointerResponse release(Button button, double x, double y);
    /// Drops a press whose release went elsewhere, without a click.
    void cancel();

    /// Zooms about the surface under the pointer. `delta` is positive to zoom in. The anchor is
    /// locked for a burst of wheel events, because the depth buffer lags the camera while zooming.
    PointerResponse scroll(double delta, double x, double y, double time);

private:
    Gesture gestureFor(Button button, u32 modifiers) const;
    void beginGesture(Gesture gesture);
    Vec3 anchorAt(double x, double y);

    Camera& camera_;
    Viewport& viewport_;
    OrbitPolicy& orbit_;
    PixelPicker picker_;
    std::vector<MouseBinding> bindings_ = defaultMouseBindings();
    ZoomSettings zoom_;
    double pixelsPerPoint_ = 1.0;

    Button pressed_ = Button::None;
    u32 pressModifiers_ = 0;
    double pressX_ = 0, pressY_ = 0;
    double lastX_ = 0, lastY_ = 0;
    Gesture active_ = Gesture::None;
    Vec3 pivot_{};
    double panDepth_ = 1.0;

    Vec3 zoomAnchor_{};
    double zoomX_ = 0, zoomY_ = 0;
    double lastZoomTime_ = -1e9;
};

} // namespace geo::nav
