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

/// Writes the section's planes into the frame, in world space, and their depths on screen.
void applySection(const Section& section, const Aabb& bounds, FrameUniforms& uniforms);

/// Writes each section plane's stored depth across the screen from the frame's view projection,
/// which must be final: the WebGPU renderer calls it again after jittering a sample.
void placeSectionDepths(FrameUniforms& uniforms);

/// How far every edge moves toward the eye at least, in points of depth at its distance: enough
/// for most of a crease line's width to clear a concave crease's faces, and to beat depth
/// precision. A definition's own chord error adds to it (GpuDefinition.edgeLift). It was 2
/// points, which, zoomed out, let the edges of everything just behind show through: on the 100k
/// rack the whole column turned to ink. At 1 point, visible creases keep 97-99.7% of their ink.
inline constexpr float kEdgeFloorPoints = 1.0f;

/// Builds the uniforms for a frame. Depth is reversed, because float precision is highest near
/// zero: distant geometry then keeps its precision. Orthographic depth hugs the scene bounds.
ViewState makeView(const nav::Camera& camera, const nav::Viewport& viewport,
                   const Aabb& sceneBounds);

} // namespace geo::render
