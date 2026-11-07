// Layouts shared by the geometry-path spike and its shaders.
#pragma once

#include <simd/simd.h>
#ifndef __METAL_VERSION__
#include <cstdint>
#endif

// Enums rather than constexpr, because they compile unchanged in both C++ and MSL.
enum SpikeLimits : unsigned {
    kMeshletMaxVertices = 124,
    kMeshletMaxTriangles = 124,
    kBatchMeshlets = 32,
};

struct PackedVertex { // 12 bytes
    uint16_t x, y, z, face;
    int16_t octX, octY;
};

struct SpikeMeshlet { // 32 bytes
    float centerX, centerY, centerZ, radius;
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint8_t vertexCount, triangleCount;
    int8_t coneX, coneY, coneZ, coneCutoff;
    uint16_t pad;
    uint32_t faceBase;
};

struct Batch {
    uint32_t firstMeshlet;
    uint32_t meshletCount;
    uint32_t instance;
    uint32_t pad;
};

struct FrameData {
    simd_float4x4 viewProjection;
    simd_float3 decodeScale;
    simd_float3 decodeOffset;
    simd_float3 cameraRight;
    simd_float3 cameraUp;
    simd_float3 cameraForward;
    simd_float3 cameraCenter;
    float halfWidth;
    float halfHeight;
};

enum BufferIndex : unsigned {
    kBufferFrame = 0,
    kBufferBatches = 1,
    kBufferMeshlets = 2,
    kBufferInstances = 3,
    kBufferBase = 4,
    kBufferVertices = 5,
    kBufferTriangles = 6,
};
