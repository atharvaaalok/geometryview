// The surface pipeline: object-stage culling, wide mesh shading and shading.
//
// Rules that keep the Apple GPU's hidden surface removal working: no discard and no depth output on
// this path, and varyings stay small. See Docs/PLAN.md.
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

// The ghost pass reuses every stage, with this constant choosing which faces it keeps.
constant bool kGhostPass [[function_constant(0)]];

struct SurfaceVertex {
    float4 position [[position]];
    float3 normal;
    uint id [[flat]];
    uint flags [[flat]];
};

struct SurfaceOutput {
    float4 color [[color(kAttachmentColor)]];
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
};

struct Payload {
    uint meshlets[kBatchMeshlets];
    uint occurrence;
};

static inline float3 transformPoint(const device GpuOccurrence& occurrence, float3 p) {
    const float4 h = float4(p, 1.0);
    return float3(dot(occurrence.rows[0], h), dot(occurrence.rows[1], h),
                  dot(occurrence.rows[2], h));
}

static inline float3 transformDirection(const device GpuOccurrence& occurrence, float3 v) {
    return float3(dot(occurrence.rows[0].xyz, v), dot(occurrence.rows[1].xyz, v),
                  dot(occurrence.rows[2].xyz, v));
}

static inline float3 decodeOctahedral(short2 encoded) {
    const float2 f = float2(encoded) / 32767.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
}

static inline bool isVisible(const device Meshlet& m, const device GpuDefinition& definition,
                             const device GpuOccurrence& occurrence,
                             constant FrameUniforms& frame) {
    const float3 local =
        float3(m.centerX, m.centerY, m.centerZ) * definition.boundsScale + definition.boundsMin;
    const float3 center = transformPoint(occurrence, local);
    const float radius = float(m.radius) * definition.boundsStep;

    const float3 offset = center - frame.eye;
    const float x = abs(dot(offset, frame.right));
    const float y = abs(dot(offset, frame.up));
    const float depth = dot(offset, frame.forward);

    if (frame.perspective != 0) {
        if (depth < -radius || x > depth * frame.tanHalfX + radius * frame.secHalfX ||
            y > depth * frame.tanHalfY + radius * frame.secHalfY) {
            return false;
        }
    } else if (x > frame.halfWidth + radius || y > frame.halfHeight + radius) {
        return false;
    }

    // An open surface shows both sides, so a normal cone proves nothing about it.
    if ((definition.flags & kDefinitionOpen) != 0) {
        return true;
    }
    const float3 axis = transformDirection(occurrence, float3(m.coneX, m.coneY, m.coneZ) / 127.0);
    const float cutoff = float(m.coneCutoff) / 127.0;
    if (frame.perspective != 0) {
        return dot(offset, axis) < cutoff * length(offset) + radius;
    }
    return dot(frame.forward, axis) < cutoff;
}

// Whether a pass draws anything of an occurrence. A face-level flag means faces must be checked one
// by one in the mesh stage.
static inline bool passKeeps(uint flags) {
    if ((flags & kStateHidden) != 0) {
        return false;
    }
    if (kGhostPass) {
        return (flags & (kStateGhost | kStateFaceGhost)) != 0;
    }
    return (flags & kStateGhost) == 0;
}

[[object, max_total_threads_per_threadgroup(1),
  max_total_threadgroups_per_mesh_grid(kBatchMeshlets)]]
void surfaceObject(object_data Payload& payload [[payload]], mesh_grid_properties grid,
                   constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                   const device GpuDefinition* definitions [[buffer(kBufferDefinitions)]],
                   const device GpuOccurrence* occurrences [[buffer(kBufferOccurrences)]],
                   const device Batch* batches [[buffer(kBufferBatches)]],
                   constant uint& drawBase [[buffer(kBufferDrawBase)]],
                   const device OccurrenceState* states [[buffer(kBufferOccurrenceState)]],
                   uint group [[threadgroup_position_in_grid]]) {
    const device Batch& batch = batches[drawBase + group];
    const device GpuOccurrence& occurrence = occurrences[batch.occurrence];
    const device GpuDefinition& definition = definitions[occurrence.definition];

    uint count = 0;
    if (!passKeeps(states[batch.occurrence].flags)) {
        grid.set_threadgroups_per_grid(uint3(0, 1, 1));
        return;
    }
    for (uint i = 0; i < batch.meshletCount; ++i) {
        const uint index = batch.firstMeshlet + i;
        if (isVisible(definition.meshlets[index], definition, occurrence, frame)) {
            payload.meshlets[count++] = index;
        }
    }
    payload.occurrence = batch.occurrence;
    grid.set_threadgroups_per_grid(uint3(count, 1, 1));
}

using SurfaceMesh = metal::mesh<SurfaceVertex, void, kMeshletMaxVertices, kMeshletMaxTriangles,
                                metal::topology::triangle>;

static inline SurfaceVertex decodeVertex(const device Meshlet& m,
                                         const device GpuDefinition& definition,
                                         const device GpuOccurrence& occurrence,
                                         OccurrenceState state, const device uchar* faceStates,
                                         constant FrameUniforms& frame, uint index) {
    const uint vertexIndex = m.vertexOffset + index;
    const device PackedVertex& v = definition.vertices[vertexIndex];

    // Integers first, so a vertex at the same grid point decodes to the same float in any meshlet,
    // which is what keeps shared edges watertight. See ADR 0004.
    uint3 grid = uint3(v.x, v.y, v.z);
    if ((definition.flags & kDefinitionWide) != 0) {
        const uint high = definition.highBits[vertexIndex];
        grid |= uint3(high & 0xFF, (high >> 8) & 0xFF, (high >> 16) & 0xFF) << 16;
    }
    const float3 local = float3(grid) * definition.decodeScale + definition.boundsMin;
    const float3 world = transformPoint(occurrence, local);
    const float3 normal = transformDirection(occurrence, decodeOctahedral(short2(v.octX, v.octY)));

    const uint face = m.faceBase + uint(v.localFace);
    const uint flags = (state.flags & (kStateHidden | kStateGhost | kStateSelected)) |
                       uint(faceStates[state.facePage + face]);
    SurfaceVertex out;
    out.position = frame.viewProjection * float4(world, 1.0);
    out.normal = frame.viewRotation * normal;
    out.id = occurrence.idBase + face + 1;
    out.flags = flags;
    // Faces never share vertices, so moving all of a face's vertices to one point outside the view
    // removes exactly that face, with no discard and no per-primitive data.
    const bool ghost = (flags & kStateGhost) != 0;
    if ((flags & kStateHidden) != 0 || ghost != kGhostPass) {
        out.position = float4(0.0, 0.0, 2.0, 1.0);
    }
    return out;
}

// One thread per primitive. Threads run in SIMD groups, which is what makes mesh shading beat the
// classic pipeline. A serial loop in one thread was 2.5 times slower. See ADR 0001.
[[mesh, max_total_threads_per_threadgroup(kMeshletMaxTriangles)]]
void surfaceMesh(SurfaceMesh out, object_data const Payload& payload [[payload]],
                 constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                 const device GpuDefinition* definitions [[buffer(kBufferDefinitions)]],
                 const device GpuOccurrence* occurrences [[buffer(kBufferOccurrences)]],
                 const device OccurrenceState* states [[buffer(kBufferOccurrenceState)]],
                 const device uchar* faceStates [[buffer(kBufferFaceState)]],
                 uint index [[thread_index_in_threadgroup]],
                 uint group [[threadgroup_position_in_grid]]) {
    const device GpuOccurrence& occurrence = occurrences[payload.occurrence];
    const device GpuDefinition& definition = definitions[occurrence.definition];
    const device Meshlet& m = definition.meshlets[payload.meshlets[group]];
    const OccurrenceState state = states[payload.occurrence];

    if (index == 0) {
        out.set_primitive_count(m.triangleCount);
    }
    if (index < m.vertexCount) {
        out.set_vertex(index,
                       decodeVertex(m, definition, occurrence, state, faceStates, frame, index));
    }
    if (index < m.triangleCount) {
        // Clamped because a cache file is untrusted and per-index validation would read every page.
        const uint last = m.vertexCount - 1;
        const uint packed = definition.triangles[m.triangleOffset + index];
        out.set_index(index * 3 + 0, min(packed & 0xFF, last));
        out.set_index(index * 3 + 1, min((packed >> 8) & 0xFF, last));
        out.set_index(index * 3 + 2, min((packed >> 16) & 0xFF, last));
    }
}

// Three flat tones with a soft step between them, from the printed-figure look. The step widens
// with the normal's rate of change, so curved parts stay smooth and flat faces keep crisp bands.
static inline float3 band3(float d, float hi, float lo, float soft, float3 light, float3 mid,
                           float3 dark) {
    const float a = smoothstep(hi - soft, hi + soft, d);
    const float b = smoothstep(lo - soft, lo + soft, d);
    return a > 0.0 ? mix(mid, light, a) : mix(dark, mid, b);
}

constant float3 kLightDirection = float3(0.561, 0.809, 0.175);
constant float3 kMetalLight = float3(0xF3, 0xF1, 0xEC) / 255.0;
constant float3 kMetalMid = float3(0xD6, 0xD2, 0xC8) / 255.0;
constant float3 kMetalDark = float3(0xA8, 0xA3, 0x98) / 255.0;
constant float3 kSelectedLight = float3(0xCD, 0xDC, 0xF6) / 255.0;
constant float3 kSelectedMid = float3(0xA6, 0xC1, 0xEF) / 255.0;
constant float3 kSelectedDark = float3(0x7A, 0x9F, 0xDC) / 255.0;
constant float3 kHover = float3(0xFF, 0xF4, 0xD6) / 255.0;
constant float kHoverAmount = 0.45;
constant float3 kGhostTint = float3(0x23, 0x1E, 0x16) / 255.0; // ink
constant float kGhostAlpha = 0.24;

fragment SurfaceOutput surfaceFragment(SurfaceVertex in [[stage_in]],
                                       bool frontFacing [[front_facing]],
                                       constant FrameUniforms& frame [[buffer(kBufferFrame)]]) {
    const float3 n = normalize(frontFacing ? in.normal : -in.normal);
    const float d = dot(n, kLightDirection);
    const float soft = clamp(1.5 * fwidth(d), 0.02, 0.18);

    const bool selected = (in.flags & kStateSelected) != 0;
    const bool hovered = in.id >= frame.hoverFirst && in.id < frame.hoverEnd;
    float3 color = selected
                       ? band3(d, -0.15, -0.45, soft, kSelectedLight, kSelectedMid, kSelectedDark)
                       : band3(d, -0.15, -0.45, soft, kMetalLight, kMetalMid, kMetalDark);
    if (hovered) {
        color = mix(color, kHover, kHoverAmount * (selected ? 0.5 : 1.0));
    }
    if (kGhostPass) {
        // A faint shell of the part, so what lies behind it stays readable.
        const float shade = mix(0.55, 0.85, saturate(d * 0.5 + 0.5));
        return {float4(mix(kGhostTint, color, shade), hovered ? kGhostAlpha * 1.6 : kGhostAlpha),
                in.id, in.position.z};
    }
    return {float4(color, 1.0), in.id, in.position.z};
}

// Copies each pixel's ID and depth to memory the CPU reads directly, so hover and pivots need no
// GPU round trip. The attachments themselves stay memoryless. Sample 0 is enough for picking.
struct PickTile {
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
};

kernel void resolvePick(imageblock<PickTile, imageblock_layout_implicit> tile,
                        texture2d<uint, access::write> ids [[texture(kTexturePickIds)]],
                        texture2d<float, access::write> depths [[texture(kTexturePickDepths)]],
                        ushort2 pixel [[thread_position_in_threadgroup]],
                        ushort2 tileIndex [[threadgroup_position_in_grid]]) {
    const uint2 position = uint2(tileIndex) * uint(kTileSize) + uint2(pixel);
    if (position.x >= ids.get_width() || position.y >= ids.get_height()) {
        return;
    }
    const PickTile sample = tile.read(pixel, 0, imageblock_data_rate::sample);
    ids.write(uint4(sample.id), position);
    depths.write(float4(sample.depth), position);
}
