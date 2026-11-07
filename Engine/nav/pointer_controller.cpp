#include "nav/pointer_controller.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace geo::nav {
namespace {

constexpr double kDragThresholdPoints = 5.0;
constexpr double kRotateSpeed = 3.0;
constexpr double kDollyPerPixel = 0.005;
constexpr double kZoomPerUnit = 0.0018;
constexpr double kZoomBurstSeconds = 0.25;

} // namespace

std::vector<MouseBinding> defaultMouseBindings() {
    return {
        {Button::Left, 0, Gesture::Rotate},
        {Button::Middle, 0, Gesture::Pan},
        {Button::Right, 0, Gesture::Dolly},
        // A MacBook has no middle button, so these give it pan.
        {Button::Left, kModifierShift, Gesture::Pan},
        {Button::Left, kModifierSpace, Gesture::Pan},
    };
}

PointerController::PointerController(Camera& camera, Viewport& viewport, OrbitPolicy& orbit,
                                     PixelPicker picker)
    : camera_(camera), viewport_(viewport), orbit_(orbit), picker_(std::move(picker)) {}

Gesture PointerController::gestureFor(Button button, u32 modifiers) const {
    Gesture best = Gesture::None;
    int bestSpecificity = -1;
    for (const MouseBinding& binding : bindings_) {
        if (binding.button != button || (binding.modifiers & ~modifiers) != 0) {
            continue;
        }
        const int specificity = std::popcount(binding.modifiers);
        if (specificity > bestSpecificity) {
            bestSpecificity = specificity;
            best = binding.gesture;
        }
    }
    return best;
}

PointerResponse PointerController::press(Button button, u32 modifiers, double x, double y) {
    if (pressed_ == Button::None) {
        pressed_ = button;
        pressModifiers_ = modifiers;
        pressX_ = lastX_ = x;
        pressY_ = lastY_ = y;
    }
    return {};
}

void PointerController::beginGesture(Gesture gesture) {
    active_ = gesture;
    const double ndcX = 2.0 * pressX_ / viewport_.width - 1.0;
    const double ndcY = 1.0 - 2.0 * pressY_ / viewport_.height;

    if (gesture == Gesture::Rotate) {
        // Resolved once, so one drag can never change its pivot partway through.
        const SurfaceProbe probe = [this](double sampleX, double sampleY) -> std::optional<Vec3> {
            const double pixelX = (sampleX + 1.0) * 0.5 * viewport_.width;
            const double pixelY = (1.0 - sampleY) * 0.5 * viewport_.height;
            const std::optional<SurfaceHit> hit = picker_(pixelX, pixelY);
            return hit ? std::optional<Vec3>(hit->point) : std::nullopt;
        };
        pivot_ = orbit_.resolve(camera_, viewport_, ndcX, ndcY, probe);
    } else if (gesture == Gesture::Pan) {
        const std::optional<SurfaceHit> hit = picker_(pressX_, pressY_);
        panDepth_ = hit ? hit->viewDepth : camera_.focusDistance;
    }
}

PointerResponse PointerController::move(double x, double y) {
    PointerResponse response;
    if (pressed_ == Button::None) {
        return response;
    }
    const double dx = x - lastX_;
    const double dy = y - lastY_;

    if (active_ == Gesture::None) {
        const double threshold = kDragThresholdPoints * pixelsPerPoint_;
        if (std::hypot(x - pressX_, y - pressY_) < threshold) {
            return response;
        }
        const Gesture gesture = gestureFor(pressed_, pressModifiers_);
        if (gesture == Gesture::None) {
            return response;
        }
        beginGesture(gesture);
    }

    switch (active_) {
    case Gesture::Rotate:
        // Both over width, so horizontal and vertical drags turn at the same rate.
        tumble(camera_, pivot_, 2.0 * dx / viewport_.width, -2.0 * dy / viewport_.width,
               kRotateSpeed);
        break;
    case Gesture::Pan:
        pan(camera_, dx, dy, panDepth_, viewport_);
        orbit_.navigated();
        break;
    case Gesture::Dolly:
        zoomAbout(camera_, std::exp(dy * kDollyPerPixel), camera_.target(), 0.0, 0.0, viewport_);
        orbit_.navigated();
        break;
    case Gesture::None:
        break;
    }
    lastX_ = x;
    lastY_ = y;
    response.cameraMoved = true;
    return response;
}

PointerResponse PointerController::release(Button button, double x, double y) {
    PointerResponse response;
    if (button != pressed_) {
        return response;
    }
    if (active_ == Gesture::None && button == Button::Left) {
        response.clicked = true;
        if (const std::optional<SurfaceHit> hit = picker_(x, y)) {
            response.clickedId = hit->id;
        }
    }
    pressed_ = Button::None;
    active_ = Gesture::None;
    return response;
}

Vec3 PointerController::anchorAt(double x, double y) {
    const double ndcX = 2.0 * x / viewport_.width - 1.0;
    const double ndcY = 1.0 - 2.0 * y / viewport_.height;
    if (const std::optional<SurfaceHit> hit = picker_(x, y)) {
        return hit->point;
    }
    return camera_.unproject(ndcX, ndcY, camera_.focusDistance, viewport_);
}

PointerResponse PointerController::scroll(double delta, double x, double y, double time) {
    const bool newBurst = time - lastZoomTime_ > kZoomBurstSeconds || x != zoomX_ || y != zoomY_;
    if (newBurst) {
        zoomAnchor_ = anchorAt(x, y);
        zoomX_ = x;
        zoomY_ = y;
    }
    lastZoomTime_ = time;

    const double ndcX = 2.0 * x / viewport_.width - 1.0;
    const double ndcY = 1.0 - 2.0 * y / viewport_.height;
    zoomAbout(camera_, std::exp(-delta * kZoomPerUnit), zoomAnchor_, ndcX, ndcY, viewport_);
    orbit_.navigated();
    PointerResponse response;
    response.cameraMoved = true;
    return response;
}

} // namespace geo::nav
