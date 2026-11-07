// Runtime records shared by the renderer and its shaders. This header compiles as both C++ and Metal
// Shading Language, so the two can never disagree about a layout.
#pragma once

#include "format/gpu_records.h"
#include <simd/simd.h>

#ifdef __METAL_VERSION__
#define GEO_DEVICE_POINTER(Type) const device Type*
#else
#define GEO_DEVICE_POINTER(Type) uint64_t
#endif

enum SurfaceLimits : unsigned {
    kBatchMeshlets = 32,
    // One dispatch of 120k threadgroups was seen to draw nothing, so draws stay well below that.
    kMaxBatchesPerDraw = 4096,
    kTileSize = 16, // 4x MSAA does not support 32x32 tiles
};

enum BufferIndex : unsigned {
    kBufferFrame = 0,
    kBufferDefinitions = 1,
    kBufferOccurrences = 2,
    kBufferBatches = 3,
    kBufferDrawBase = 4,
};

enum TextureIndex : unsigned {
    kTexturePickIds = 0,
    kTexturePickDepths = 1,
};

enum AttachmentIndex : unsigned {
    kAttachmentColor = 0,
    kAttachmentId = 1,
    kAttachmentDepth = 2,
};

// One per definition. Geometry is addressed directly, so a scene can span several mapped files.
struct GpuDefinition {
    GEO_DEVICE_POINTER(Meshlet) meshlets;
    GEO_DEVICE_POINTER(PackedVertex) vertices;
    GEO_DEVICE_POINTER(uint32_t) highBits;
    GEO_DEVICE_POINTER(uint32_t) triangles;
    simd_float3 boundsMin;
    simd_float3 decodeScale;
    simd_float3 boundsScale;
    float boundsStep; // the largest component of boundsScale
    uint32_t flags;
};

// One placed instance of a definition.
struct GpuOccurrence {
    simd_float4 rows[3]; // a 3x4 transform by rows
    uint32_t definition;
    uint32_t idBase; // pick ids of this occurrence start here
    uint32_t pad[2];
};

// Up to kBatchMeshlets meshlets of one occurrence, culled together by one object thread.
struct Batch {
    uint32_t occurrence;
    uint32_t firstMeshlet;
    uint32_t meshletCount;
    uint32_t pad;
};

struct FrameUniforms {
    simd_float4x4 viewProjection;
    simd_float3x3 viewRotation; // world to view, for lighting
    simd_float3 eye;
    simd_float3 right;
    simd_float3 up;
    simd_float3 forward;
    float halfWidth;   // orthographic half extents
    float halfHeight;
    float tanHalfX;    // perspective half-angle tangents
    float tanHalfY;
    float secHalfX;    // reciprocal cosines, for sphere tests
    float secHalfY;
    uint32_t perspective;
    uint32_t hoveredId;
    uint32_t selectedId;
    uint32_t pad;
};
