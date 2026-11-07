// Runtime records shared by the renderer and its shaders. This header compiles as both C++ and
// Metal Shading Language, so the two can never disagree about a layout.
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
    kBufferOccurrenceState = 5,
    kBufferFaceState = 6,
    kBufferMeshletRefs = 7,
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

// Flags shared by occurrences and faces. A face shows its own flags combined with its occurrence's.
enum StateFlags : uint32_t {
    kStateHidden = 1u << 0,
    kStateGhost = 1u << 1,
    kStateSelected = 1u << 2,
    // Occurrence only: some face has its own flag, so the per-face page must be read.
    kStateFaceHidden = 1u << 4,
    kStateFaceGhost = 1u << 5,
    kStateFaceSelected = 1u << 6,
};

// Per occurrence: its own flags, and where its per-face bytes start. Occurrences with no face-level
// change point at a shared page of zeros.
struct OccurrenceState {
    uint32_t flags;
    uint32_t facePage;
};

// Up to kBatchMeshlets meshlets, culled together by one object thread. A large definition's
// batches cover consecutive meshlets of one occurrence. Small definitions' meshlets are packed from
// many occurrences into shared batches through MeshletRefs, so a million one-meshlet parts need
// 31k object threadgroups rather than a million, and the driver memory that scales with them.
enum BatchKind : uint32_t {
    kBatchRun = 0,    // meshlets [first, first + count) of `occurrence`
    kBatchPacked = 1, // MeshletRefs [first, first + count)
};

struct Batch {
    uint32_t kind;
    uint32_t occurrence;
    uint32_t first;
    uint32_t count;
};

struct MeshletRef {
    uint32_t occurrence;
    uint32_t meshlet;
};

struct FrameUniforms {
    simd_float4x4 viewProjection;
    simd_float3x3 viewRotation; // world to view, for lighting
    simd_float3 eye;
    simd_float3 right;
    simd_float3 up;
    simd_float3 forward;
    float halfWidth; // orthographic half extents
    float halfHeight;
    float tanHalfX; // perspective half-angle tangents
    float tanHalfY;
    float secHalfX; // reciprocal cosines, for sphere tests
    float secHalfY;
    uint32_t perspective;
    // Hover covers a range of ids, so a whole body or part lights up in those selection modes.
    uint32_t hoverFirst;
    uint32_t hoverEnd;
    uint32_t ghostPickable; // ghosts write ids, while the ghost tool is held, so a click restores
};
