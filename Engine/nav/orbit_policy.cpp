#include "nav/orbit_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace geo::nav {
namespace {

// Size of the visible bounds on screen, where 1 means they exactly fill the view on that axis. The
// gap between the two thresholds is the hysteresis.
constexpr double kEnterCloseUp = 1.5;
constexpr double kExitCloseUp = 1.2;

struct Sample {
    double x, y;
};

// A grid around the viewport centre, nearest first.
constexpr std::array<Sample, 9> kSamples = {{{0.0, 0.0},
                                             {-0.2, 0.0},
                                             {0.2, 0.0},
                                             {0.0, -0.2},
                                             {0.0, 0.2},
                                             {-0.2, -0.2},
                                             {0.2, -0.2},
                                             {-0.2, 0.2},
                                             {0.2, 0.2}}};

} // namespace

void OrbitPolicy::setMode(PivotMode mode) {
    mode_ = mode;
    hasExplicitPivot_ = false;
    evaluate_ = true;
    pivot_ = bounds_.center();
}

void OrbitPolicy::setExplicitPivot(Vec3 pivot) {
    pivot_ = pivot;
    hasExplicitPivot_ = true;
}

void OrbitPolicy::clearExplicitPivot() {
    hasExplicitPivot_ = false;
    evaluate_ = true;
    pivot_ = bounds_.center();
}

void OrbitPolicy::setVisibleBounds(const Aabb& bounds) {
    bounds_ = bounds;
    evaluate_ = true;
    if (!hasExplicitPivot_) {
        pivot_ = bounds.center();
    }
}

void OrbitPolicy::updateCloseUp(const Camera& camera, const Viewport& viewport) {
    double minX = std::numeric_limits<double>::infinity();
    double minY = minX;
    double maxX = -minX, maxY = -minX;
    bool behind = false;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point = simd_make_double3(corner & 1 ? bounds_.max.x : bounds_.min.x,
                                             corner & 2 ? bounds_.max.y : bounds_.min.y,
                                             corner & 4 ? bounds_.max.z : bounds_.min.z);
        const NdcPoint ndc = camera.project(point, viewport);
        // An orthographic view has no vanishing point, so depth cannot invalidate its projection.
        behind = behind || (camera.perspective && ndc.viewDepth <= 0.0);
        minX = std::min(minX, ndc.x);
        maxX = std::max(maxX, ndc.x);
        minY = std::min(minY, ndc.y);
        maxY = std::max(maxY, ndc.y);
    }
    const double sizeX = (maxX - minX) * 0.5;
    const double sizeY = (maxY - minY) * 0.5;
    if (behind || sizeX > kEnterCloseUp || sizeY > kEnterCloseUp) {
        closeUp_ = true;
    } else if (sizeX < kExitCloseUp && sizeY < kExitCloseUp) {
        closeUp_ = false;
    }
}

Vec3 OrbitPolicy::resolve(const Camera& camera, const Viewport& viewport, double pointerX,
                          double pointerY, const SurfaceProbe& probe) {
    if (hasExplicitPivot_) {
        return pivot_;
    }
    if (mode_ == PivotMode::Pointer) {
        // A miss keeps the previous pivot, so dragging from empty space stays predictable.
        if (const std::optional<Vec3> hit = probe(pointerX, pointerY)) {
            pivot_ = *hit;
        }
        return pivot_;
    }
    if (mode_ == PivotMode::Visible || !evaluate_) {
        return pivot_;
    }

    evaluate_ = false;
    updateCloseUp(camera, viewport);
    if (!closeUp_) {
        pivot_ = bounds_.center();
        return pivot_;
    }
    for (const Sample& sample : kSamples) {
        if (const std::optional<Vec3> hit = probe(sample.x, sample.y)) {
            pivot_ = *hit;
            break;
        }
    }
    return pivot_;
}

} // namespace geo::nav
