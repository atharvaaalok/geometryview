// Turns a camera into the uniforms the shaders use.
#pragma once

#include "math/geometry.h"
#include "nav/camera.h"
#include "shaders/shared/gpu_types.h"

#include <array>

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

/// Cuts across the model's axes, each placed as a fraction of the bounds.
struct Section {
    std::array<bool, 3> enabled{};
    std::array<float, 3> position{0.5f, 0.5f, 0.5f};
    std::array<bool, 3> flip{};

    bool any() const { return enabled[0] || enabled[1] || enabled[2]; }
};

/// Writes the section's planes into the frame, in world space.
void applySection(const Section& section, const Aabb& bounds, FrameUniforms& uniforms);

/// Builds the uniforms for a frame. Depth is reversed, because float precision is highest near
/// zero: distant geometry then keeps its precision. Orthographic depth hugs the scene bounds.
ViewState makeView(const nav::Camera& camera, const nav::Viewport& viewport,
                   const Aabb& sceneBounds);

} // namespace geo::render
