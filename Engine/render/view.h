// Turns a camera into the uniforms the shaders use.
#pragma once

#include "math/geometry.h"
#include "nav/camera.h"
#include "shaders/shared/gpu_types.h"

namespace geo::render {

/// Converts depth attachment values back to distance along the camera's forward axis.
struct DepthDecoder {
    bool perspective = false;
    double nearPlane = 1.0;
    double farPlane = 2.0;

    double viewDepth(float depth) const;
};

struct ViewState {
    FrameUniforms uniforms;
    DepthDecoder depth;
};

/// Builds the uniforms for a frame. Depth is reversed, because float precision is highest near zero:
/// distant geometry then keeps its precision. Orthographic depth hugs the scene bounds.
ViewState makeView(const nav::Camera& camera, const nav::Viewport& viewport,
                   const Aabb& sceneBounds);

} // namespace geo::render
