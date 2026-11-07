// The view cube and the scale bar. Both draw over the model at the end of the main pass, in
// lockstep with the camera, with no depth test.
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

struct OverlayOutput {
    float4 color [[color(kAttachmentColor)]];
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
};

// Cube faces with Z up: the outward normal, then the label's right and up directions. The order
// matches nav::StandardView's faces: front, back, left, right, top, bottom.
constant float3 kFaceNormal[6] = {float3(0, -1, 0), float3(0, 1, 0), float3(-1, 0, 0),
                                  float3(1, 0, 0),  float3(0, 0, 1), float3(0, 0, -1)};
constant float3 kFaceRight[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, -1, 0),
                                 float3(0, 1, 0), float3(1, 0, 0),  float3(1, 0, 0)};
constant float3 kFaceUp[6] = {float3(0, 0, 1), float3(0, 0, 1), float3(0, 0, 1),
                              float3(0, 0, 1), float3(0, 1, 0), float3(0, -1, 0)};
constant float2 kQuadCorner[6] = {float2(-1, -1), float2(1, -1), float2(1, 1),
                                  float2(-1, -1), float2(1, 1),  float2(-1, 1)};

constant float3 kLight = float3(0.561, 0.809, 0.175);
constant float3 kLightTone = float3(0xF3, 0xF1, 0xEC) / 255.0;
constant float3 kMidTone = float3(0xD6, 0xD2, 0xC8) / 255.0;
constant float3 kDarkTone = float3(0xBF, 0xBA, 0xAE) / 255.0;
constant float3 kInk = float3(0x23, 0x1E, 0x16) / 255.0;
constant float3 kCream = float3(0xFF, 0xF4, 0xD6) / 255.0;

struct CubeVertex {
    float4 position [[position]];
    float2 local; // -1 to 1 across the face
    float3 normal [[flat]];
    uint face [[flat]];
};

static float4 toClip(float2 pixel, float2 viewport) {
    return float4(pixel.x / viewport.x * 2.0 - 1.0, 1.0 - pixel.y / viewport.y * 2.0, 0.0, 1.0);
}

vertex CubeVertex cubeVertex(uint vertexId [[vertex_id]],
                             constant OverlayUniforms& u [[buffer(kOverlayUniforms)]]) {
    const uint face = vertexId / 6;
    const float2 corner = kQuadCorner[vertexId % 6];
    const float3 world = kFaceNormal[face] + kFaceRight[face] * corner.x + kFaceUp[face] * corner.y;
    const float3 view = u.viewRotation * world;
    const float3 normal = u.viewRotation * kFaceNormal[face];

    CubeVertex out;
    out.position = toClip(u.cubeCenter + float2(view.x, -view.y) * u.cubeHalf, u.viewport);
    // Faces turned away are dropped; the cube is convex, so no depth test is needed.
    if (normal.z <= 1e-4) {
        out.position = float4(0.0, 0.0, 2.0, 1.0);
    }
    out.local = corner;
    out.normal = normal;
    out.face = face;
    return out;
}

fragment OverlayOutput cubeFragment(CubeVertex in [[stage_in]],
                                    constant OverlayUniforms& u [[buffer(kOverlayUniforms)]],
                                    texture2d<float> atlas [[texture(0)]]) {
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    const float d = dot(normalize(in.normal), kLight);
    float3 color = d > -0.15 ? kLightTone : (d > -0.45 ? kMidTone : kDarkTone);
    if (u.cubeHover == in.face + 1) {
        color = kCream;
    }

    // The label fills the middle of the face, from its row of the atlas.
    const float2 labelHalf = float2(0.9, 0.3);
    const float2 inLabel = in.local / labelHalf;
    if (all(abs(inLabel) <= 1.0)) {
        const float2 cell = float2(kAtlasLabelWidth, kAtlasLabelHeight);
        const float2 texel = float2((inLabel.x * 0.5 + 0.5) * cell.x,
                                    (0.5 - inLabel.y * 0.5) * cell.y + in.face * cell.y);
        const float coverage = atlas.sample(linear, texel / float2(kAtlasWidth, kAtlasHeight)).r;
        color = mix(color, kInk, coverage);
    }

    // An ink edge of constant width in pixels, from how fast the face coordinate changes.
    const float2 edge = (1.0 - abs(in.local)) / max(fwidth(in.local), float2(1e-5));
    const float ink = 1.0 - smoothstep(u.lineWidth - 0.5, u.lineWidth + 0.5, min(edge.x, edge.y));
    color = mix(color, kInk, ink);
    return {float4(color, 1.0), kCubeIdBase + in.face, 0.0};
}

struct QuadVertex {
    float4 position [[position]];
    float2 uv;
    float4 color [[flat]];
    bool solid [[flat]];
};

vertex QuadVertex quadVertex(uint vertexId [[vertex_id]], uint instance [[instance_id]],
                             constant OverlayUniforms& u [[buffer(kOverlayUniforms)]],
                             const device OverlayQuad* quads [[buffer(kOverlayQuads)]]) {
    const OverlayQuad quad = quads[instance];
    const float2 corner = kQuadCorner[vertexId] * 0.5 + 0.5;
    QuadVertex out;
    out.position = toClip(quad.origin + corner * quad.size, u.viewport);
    out.solid = quad.uv.z < 0.0;
    out.uv = mix(quad.uv.xy, quad.uv.zw, float2(corner.x, 1.0 - corner.y));
    out.color = quad.color;
    return out;
}

fragment OverlayOutput quadFragment(QuadVertex in [[stage_in]],
                                    texture2d<float> atlas [[texture(0)]]) {
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    const float coverage = in.solid ? 1.0 : atlas.sample(linear, in.uv).r;
    return {float4(in.color.rgb, in.color.a * coverage), 0u, 0.0};
}
