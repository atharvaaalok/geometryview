#include "render/view.h"

#include <algorithm>
#include <cmath>

namespace geo::render {
namespace {

simd_float3 toFloat(Vec3 v) {
    return simd_make_float3(float(v.x), float(v.y), float(v.z));
}

simd_float4x4 viewMatrix(Vec3 right, Vec3 up, Vec3 back, Vec3 eye) {
    simd_float4x4 view = matrix_identity_float4x4;
    view.columns[0] = simd_make_float4(float(right.x), float(up.x), float(back.x), 0.0f);
    view.columns[1] = simd_make_float4(float(right.y), float(up.y), float(back.y), 0.0f);
    view.columns[2] = simd_make_float4(float(right.z), float(up.z), float(back.z), 0.0f);
    view.columns[3] = simd_make_float4(float(-simd_dot(right, eye)), float(-simd_dot(up, eye)),
                                       float(-simd_dot(back, eye)), 1.0f);
    return view;
}

} // namespace

double DepthDecoder::viewDepth(float depth) const {
    if (perspective) {
        return depth > 0.0f ? nearPlane / double(depth) : farPlane;
    }
    return farPlane - double(depth) * (farPlane - nearPlane);
}

ViewState makeView(const nav::Camera& camera, const nav::Viewport& viewport,
                   const Aabb& sceneBounds) {
    const Vec3 right = camera.right(), up = camera.up(), back = camera.back();
    const Vec3 forward = -back;
    const double aspect = viewport.aspect();
    const double radius = std::max(sceneBounds.radius(), 1e-9);
    const double centerDepth = simd_dot(sceneBounds.center() - camera.position, forward);

    ViewState view{};
    FrameUniforms& u = view.uniforms;
    u.viewRotation = simd_matrix(simd_make_float3(float(right.x), float(up.x), float(back.x)),
                                 simd_make_float3(float(right.y), float(up.y), float(back.y)),
                                 simd_make_float3(float(right.z), float(up.z), float(back.z)));
    u.eye = toFloat(camera.position);
    u.right = toFloat(right);
    u.up = toFloat(up);
    u.forward = toFloat(forward);
    u.perspective = camera.perspective ? 1 : 0;
    u.viewport = simd_make_float2(float(viewport.width), float(viewport.height));

    simd_float4x4 projection{};
    if (camera.perspective) {
        const double tanY = std::tan(camera.fovY * 0.5);
        const double tanX = tanY * aspect;
        // An infinite far plane, with the near plane close to the nearest possible geometry.
        const double nearPlane = std::max(centerDepth - radius, radius * 1e-4);
        view.depth = {true, nearPlane, radius * 1e6};
        projection.columns[0].x = float(1.0 / tanX);
        projection.columns[1].y = float(1.0 / tanY);
        projection.columns[2].w = -1.0f;
        projection.columns[3].z = float(nearPlane);
        u.tanHalfX = float(tanX);
        u.tanHalfY = float(tanY);
        u.secHalfX = float(std::sqrt(1.0 + tanX * tanX));
        u.secHalfY = float(std::sqrt(1.0 + tanY * tanY));
    } else {
        const double halfHeight = camera.orthoHalfHeight;
        const double halfWidth = halfHeight * aspect;
        const double padding = radius * 0.01;
        const double nearPlane = centerDepth - radius - padding;
        const double farPlane = centerDepth + radius + padding;
        view.depth = {false, nearPlane, farPlane};
        projection.columns[0].x = float(1.0 / halfWidth);
        projection.columns[1].y = float(1.0 / halfHeight);
        projection.columns[2].z = float(1.0 / (farPlane - nearPlane));
        projection.columns[3].z = float(farPlane / (farPlane - nearPlane));
        projection.columns[3].w = 1.0f;
        u.halfWidth = float(halfWidth);
        u.halfHeight = float(halfHeight);
    }
    u.viewProjection = simd_mul(projection, viewMatrix(right, up, back, camera.position));
    return view;
}

} // namespace geo::render
