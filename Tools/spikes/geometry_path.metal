// Shaders for the geometry-path spike. Classic and mesh paths share all shading code, so any time
// difference comes from geometry processing.
#include "geometry_path_shared.h"
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    uint face [[flat]];
};

struct FragmentOut {
    float4 color [[color(0)]];
    uint id [[color(1)]];
};

static inline float3 decodeOctahedral(short2 encoded) {
    float2 f = float2(encoded) / 32767.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}

static inline VertexOut transformVertex(PackedVertex v, constant FrameData& frame, float3 translate,
                                        uint faceBase) {
    float3 local = float3(v.x, v.y, v.z) * frame.decodeScale + frame.decodeOffset;
    VertexOut out;
    out.position = frame.viewProjection * float4(local + translate, 1.0);
    out.normal = decodeOctahedral(short2(v.octX, v.octY));
    out.face = faceBase + uint(v.face);
    return out;
}

vertex VertexOut classicVertex(uint vertexID [[vertex_id]], uint instanceID [[instance_id]],
                               constant FrameData& frame [[buffer(kBufferFrame)]],
                               const device PackedVertex* vertices [[buffer(kBufferVertices)]],
                               const device float4* instances [[buffer(kBufferInstances)]]) {
    return transformVertex(vertices[vertexID], frame, instances[instanceID].xyz, 0);
}

fragment FragmentOut surfaceFragment(VertexOut in [[stage_in]]) {
    float3 n = normalize(in.normal);
    float d = dot(n, float3(0.561, 0.809, 0.175));
    float a = smoothstep(-0.33, 0.03, d);
    float b = smoothstep(-0.63, -0.27, d);
    float3 light = float3(0.957, 0.722, 0.161);
    float3 mid = float3(0.831, 0.612, 0.122);
    float3 dark = float3(0.604, 0.427, 0.078);
    float3 color = a > 0.0 ? mix(mid, light, a) : mix(dark, mid, b);
    return {float4(color, 1.0), in.face + 1};
}

struct Payload {
    uint meshlets[kBatchMeshlets];
    uint instance;
};

static inline bool isVisible(const device SpikeMeshlet& m, constant FrameData& frame,
                             float3 translate) {
    float3 center = float3(m.centerX, m.centerY, m.centerZ) + translate - frame.cameraCenter;
    float x = dot(center, frame.cameraRight);
    float y = dot(center, frame.cameraUp);
    if (abs(x) > frame.halfWidth + m.radius || abs(y) > frame.halfHeight + m.radius) {
        return false;
    }
    float3 axis = float3(m.coneX, m.coneY, m.coneZ) / 127.0;
    return dot(frame.cameraForward, axis) < float(m.coneCutoff) / 127.0;
}

[[object, max_total_threads_per_threadgroup(1), max_total_threadgroups_per_mesh_grid(32)]]
void objectAll(object_data Payload& payload [[payload]], mesh_grid_properties grid,
               const device Batch* batches [[buffer(kBufferBatches)]],
               constant uint& base [[buffer(kBufferBase)]],
               uint group [[threadgroup_position_in_grid]]) {
    const device Batch& batch = batches[base + group];
    for (uint i = 0; i < batch.meshletCount; ++i) {
        payload.meshlets[i] = batch.firstMeshlet + i;
    }
    payload.instance = batch.instance;
    grid.set_threadgroups_per_grid(uint3(batch.meshletCount, 1, 1));
}

[[object, max_total_threads_per_threadgroup(1), max_total_threadgroups_per_mesh_grid(32)]]
void objectCull(object_data Payload& payload [[payload]], mesh_grid_properties grid,
                constant FrameData& frame [[buffer(kBufferFrame)]],
                const device Batch* batches [[buffer(kBufferBatches)]],
                const device SpikeMeshlet* meshlets [[buffer(kBufferMeshlets)]],
                const device float4* instances [[buffer(kBufferInstances)]],
                constant uint& base [[buffer(kBufferBase)]],
                uint group [[threadgroup_position_in_grid]]) {
    const device Batch& batch = batches[base + group];
    float3 translate = instances[batch.instance].xyz;
    uint count = 0;
    for (uint i = 0; i < batch.meshletCount; ++i) {
        uint index = batch.firstMeshlet + i;
        if (isVisible(meshlets[index], frame, translate)) {
            payload.meshlets[count++] = index;
        }
    }
    payload.instance = batch.instance;
    grid.set_threadgroups_per_grid(uint3(count, 1, 1));
}

using MeshOut = metal::mesh<VertexOut, void, kMeshletMaxVertices, kMeshletMaxTriangles,
                            metal::topology::triangle>;

// One thread per primitive: threads run in SIMD groups, which is what makes mesh shading compete.
[[mesh, max_total_threads_per_threadgroup(kMeshletMaxTriangles)]]
void meshWide(MeshOut out, object_data const Payload& payload [[payload]],
              constant FrameData& frame [[buffer(kBufferFrame)]],
              const device SpikeMeshlet* meshlets [[buffer(kBufferMeshlets)]],
              const device float4* instances [[buffer(kBufferInstances)]],
              const device PackedVertex* vertices [[buffer(kBufferVertices)]],
              const device uchar4* triangles [[buffer(kBufferTriangles)]],
              uint index [[thread_index_in_threadgroup]],
              uint group [[threadgroup_position_in_grid]]) {
    const device SpikeMeshlet& m = meshlets[payload.meshlets[group]];
    if (index == 0) {
        out.set_primitive_count(m.triangleCount);
    }
    if (index < m.vertexCount) {
        float3 translate = instances[payload.instance].xyz;
        out.set_vertex(
            index, transformVertex(vertices[m.vertexOffset + index], frame, translate, m.faceBase));
    }
    if (index < m.triangleCount) {
        uchar4 t = triangles[m.triangleOffset + index];
        out.set_index(index * 3 + 0, t.x);
        out.set_index(index * 3 + 1, t.y);
        out.set_index(index * 3 + 2, t.z);
    }
}
