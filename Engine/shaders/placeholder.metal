#include <metal_stdlib>
using namespace metal;

vertex float4 placeholderVertex(uint vertexID [[vertex_id]]) {
    return float4(float2(vertexID & 1, vertexID >> 1) * 2.0 - 1.0, 0.0, 1.0);
}
