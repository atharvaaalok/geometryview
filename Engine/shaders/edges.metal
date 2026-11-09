// Feature edges as screen-space quads, one SIMD group of segments per mesh threadgroup. Each
// segment reads the state of the faces on its sides to decide what it is: a crease on something
// solid, a highlight around a hovered or selected face, or a dashed line of a ghost. Coverage goes
// to the ink attachment, keeping the larger value, so overlapping quads at a joint never ink twice.
// The tile resolve at the end of the pass inks the colour from it.
#include "geometry.h"
#include "noise.h"
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

// The ghost-lines pass draws ghosts' dashed edges, after ghosts' skins. The other pass draws
// creases and highlights, before them.
constant bool kGhostLinePass [[function_constant(2)]];
// Section planes are on, so lines are clipped with the surfaces.
constant bool kSection [[function_constant(4)]];

constant uint kNoOccurrence = 0xFFFFFFFF;
// Edges of a chunk smaller than this on screen, in pixels of radius, would only blot it with ink,
// so they are left out; the outline from the tags still draws the part's shape. This is also what
// keeps a million small parts cheap.
constant float kMinChunkPixels = 3.0;
constant uint kInked = 1;       // a crease, or a ghost's line
constant uint kHighlighted = 2; // around a hovered or selected face

struct EdgePayload {
    uint visible[kBatchMeshlets]; // edge chunks of the run's occurrence, or indices of refs
    uint occurrence;
};

struct EdgeVertex {
    float4 position [[position]];
    float clip [[clip_distance, function_constant(kSection)]][kMaxSectionPlanes];
    float across [[center_no_perspective]]; // pixels from the line's middle, signed
    float along [[center_no_perspective]];  // pixels along the edge's chain, for dashes
    uint kind [[flat]];
};

struct EdgeFragmentIn {
    float4 position [[position]];
    float across [[center_no_perspective]];
    float along [[center_no_perspective]];
    uint kind [[flat]];
};

struct InkOutput {
    float4 ink [[color(kAttachmentInk)]];
};

// The wobble the composite applies to everything it samples, so lines follow it. See noise.h.
static inline float2 wobbleOffset(float2 pixel, constant StyleUniforms& style) {
    if (style.wobble <= 0.0) {
        return float2(0.0);
    }
    const float2 q = pixel / style.wobblePeriod;
    return style.wobble * 2.0 *
           float2(valueNoise(q) - 0.5, valueNoise(q + float2(17.0, 9.0)) - 0.5);
}

[[object, max_total_threads_per_threadgroup(1),
  max_total_threadgroups_per_mesh_grid(kBatchMeshlets)]]
void edgeObject(object_data EdgePayload& payload [[payload]], mesh_grid_properties grid,
                constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                constant StyleUniforms& style [[buffer(kBufferStyle)]],
                const device GpuDefinition* definitions [[buffer(kBufferDefinitions)]],
                const device GpuOccurrence* occurrences [[buffer(kBufferOccurrences)]],
                const device Batch* batches [[buffer(kBufferBatches)]],
                constant uint& drawBase [[buffer(kBufferDrawBase)]],
                const device OccurrenceState* states [[buffer(kBufferOccurrenceState)]],
                const device MeshletRef* refs [[buffer(kBufferMeshletRefs)]],
                uint group [[threadgroup_position_in_grid]]) {
    // Whether this pass can draw anything of an occurrence. Faces are checked one by one later.
    const auto keeps = [&](uint flags, const device GpuOccurrence& occurrence) {
        if ((flags & kStateHidden) != 0) {
            return false;
        }
        if (kGhostLinePass) {
            return (style.ghostStyle & kGhostLines) != 0 &&
                   (flags & (kStateGhost | kStateFaceGhost)) != 0;
        }
        if (style.crease > 0.0 || (flags & (kStateSelected | kStateFaceSelected)) != 0) {
            return true;
        }
        // Without creases only highlights draw: a selected part, or the one under the pointer.
        const uint first = occurrence.idBase + 1;
        const uint end = first + definitions[occurrence.definition].faceCount;
        return frame.hoverEnd > frame.hoverFirst && frame.hoverFirst < end && frame.hoverEnd > first;
    };
    const auto inView = [&](const device GpuOccurrence& occurrence, uint chunk) {
        const device GpuDefinition& definition = definitions[occurrence.definition];
        const device EdgeChunk& c = definition.edgeChunks[chunk];
        const float3 center =
            boundsCenter(definition, occurrence, ushort3(c.centerX, c.centerY, c.centerZ));
        const float radius = float(c.radius) * definition.boundsStep;
        const float pixelWorld =
            frame.perspective != 0
                ? frame.worldPerPixel * max(dot(center - frame.eye, frame.forward), radius)
                : frame.worldPerPixel;
        return radius >= kMinChunkPixels * pixelWorld && sphereInView(center, radius, frame);
    };

    const device Batch& batch = batches[drawBase + group];
    uint count = 0;
    if (batch.kind == kBatchRun) {
        const device GpuOccurrence& occurrence = occurrences[batch.occurrence];
        if (keeps(states[batch.occurrence].flags, occurrence)) {
            for (uint i = 0; i < batch.count; ++i) {
                if (inView(occurrence, batch.first + i)) {
                    payload.visible[count++] = batch.first + i;
                }
            }
        }
    } else {
        for (uint i = 0; i < batch.count; ++i) {
            const MeshletRef ref = refs[batch.first + i];
            if (keeps(states[ref.occurrence].flags, occurrences[ref.occurrence]) &&
                inView(occurrences[ref.occurrence], ref.meshlet)) {
                payload.visible[count++] = batch.first + i;
            }
        }
    }
    payload.occurrence = batch.kind == kBatchRun ? batch.occurrence : kNoOccurrence;
    grid.set_threadgroups_per_grid(uint3(count, 1, 1));
}

using EdgeMesh = metal::mesh<EdgeVertex, void, kEdgeChunkSegments * 4, kEdgeChunkSegments * 2,
                             metal::topology::triangle>;

// One thread per segment. A segment that draws nothing still emits its two triangles, collapsed
// outside the view.
[[mesh, max_total_threads_per_threadgroup(kEdgeChunkSegments)]]
void edgeMesh(EdgeMesh out, object_data const EdgePayload& payload [[payload]],
              constant FrameUniforms& frame [[buffer(kBufferFrame)]],
              constant StyleUniforms& style [[buffer(kBufferStyle)]],
              const device GpuDefinition* definitions [[buffer(kBufferDefinitions)]],
              const device GpuOccurrence* occurrences [[buffer(kBufferOccurrences)]],
              const device OccurrenceState* states [[buffer(kBufferOccurrenceState)]],
              const device uchar* faceStates [[buffer(kBufferFaceState)]],
              const device MeshletRef* refs [[buffer(kBufferMeshletRefs)]],
              uint index [[thread_index_in_threadgroup]],
              uint group [[threadgroup_position_in_grid]]) {
    const MeshletRef ref = payload.occurrence != kNoOccurrence
                               ? MeshletRef{payload.occurrence, payload.visible[group]}
                               : refs[payload.visible[group]];
    const device GpuOccurrence& occurrence = occurrences[ref.occurrence];
    const device GpuDefinition& definition = definitions[occurrence.definition];
    const device EdgeChunk& chunk = definition.edgeChunks[ref.meshlet];
    const uint segments = min(chunk.segmentCount, uint(kEdgeChunkSegments));
    if (index == 0) {
        out.set_primitive_count(segments * 2);
    }
    if (index >= segments) {
        return;
    }
    const uint first = index * 4;
    out.set_index(index * 6 + 0, first);
    out.set_index(index * 6 + 1, first + 1);
    out.set_index(index * 6 + 2, first + 2);
    out.set_index(index * 6 + 3, first + 2);
    out.set_index(index * 6 + 4, first + 1);
    out.set_index(index * 6 + 5, first + 3);

    // A cache file is untrusted, so what it says is clamped rather than read out of bounds.
    const EdgeSegment segment = definition.edgeSegments[chunk.firstSegment + index];
    const uint lastFace = max(definition.faceCount, 1u) - 1;
    const uint lastVertex = max(definition.vertexCount, 1u) - 1;
    const uint edgeClass = segment.faceB >> kEdgeClassShift;
    const uint faceA = min(segment.faceA, lastFace);
    const uint rawB = segment.faceB & kEdgeNoFace;
    const uint faceB = rawB == kEdgeNoFace ? faceA : min(rawB, lastFace);

    const OccurrenceState state = states[ref.occurrence];
    const uint flagsA = faceFlags(state, faceStates, faceA);
    const uint flagsB = faceFlags(state, faceStates, faceB);
    const bool shownA = (flagsA & kStateHidden) == 0;
    const bool shownB = (flagsB & kStateHidden) == 0;
    const bool solid =
        (shownA && (flagsA & kStateGhost) == 0) || (shownB && (flagsB & kStateGhost) == 0);
    const uint idA = occurrence.idBase + faceA + 1, idB = occurrence.idBase + faceB + 1;
    const bool hovered = (shownA && idA >= frame.hoverFirst && idA < frame.hoverEnd) ||
                         (shownB && idB >= frame.hoverFirst && idB < frame.hoverEnd);
    const bool selected =
        (shownA && (flagsA & kStateSelected) != 0) || (shownB && (flagsB & kStateSelected) != 0);

    uint kind = 0;
    if (kGhostLinePass) {
        kind = !solid && (shownA || shownB) && edgeClass != kEdgeSmooth ? kInked : 0;
    } else {
        kind = (solid && edgeClass != kEdgeSmooth && style.crease > 0.0 ? kInked : 0) |
               (hovered || selected ? kHighlighted : 0);
    }

    EdgeVertex v;
    v.position = float4(0.0, 0.0, 2.0, 1.0);
    v.across = 0.0;
    v.along = 0.0;
    v.kind = 0;
    if (kSection) {
        for (uint k = 0; k < kMaxSectionPlanes; ++k) {
            v.clip[k] = 1.0;
        }
    }

    const float3 a =
        transformPoint(occurrence, decodePosition(definition, min(segment.a, lastVertex)));
    const float3 b =
        transformPoint(occurrence, decodePosition(definition, min(segment.b, lastVertex)));
    // Toward the eye by the definition's chord error, or the floor if that is more: an edge lies
    // on its own faces exactly, but on a curved face the facets beside it can sit a hair in front
    // of it, and the line would show in dashes; and a line has width, which a concave crease's
    // faces would cut into. No more than that, or the edges of what is just behind show through.
    const auto pixelWorld = [&](float3 p) {
        return frame.perspective != 0
                   ? frame.worldPerPixel * max(dot(p - frame.eye, frame.forward), 0.0)
                   : frame.worldPerPixel;
    };
    const float chordLift = definition.edgeLift * metal::length(occurrence.rows[0].xyz);
    const auto lift = [&](float3 p) {
        const float offset =
            max(chordLift, frame.edgeFloor * style.pixelsPerPoint * pixelWorld(p));
        const float3 toward = frame.perspective != 0 ? normalize(frame.eye - p) : -frame.forward;
        return p + toward * offset;
    };
    const float4 clipA = frame.viewProjection * float4(lift(a), 1.0);
    const float4 clipB = frame.viewProjection * float4(lift(b), 1.0);
    if (kind == 0 || clipA.w <= 1e-6 || clipB.w <= 1e-6) {
        for (uint k = 0; k < 4; ++k) {
            out.set_vertex(first + k, v);
        }
        return;
    }

    const float2 halfViewport = 0.5 * frame.viewport;
    const float2 screenA = clipA.xy / clipA.w * halfViewport;
    const float2 screenB = clipB.xy / clipB.w * halfViewport;
    const float2 span = screenB - screenA;
    const float length = metal::length(span);
    const float2 direction = length > 1e-4 ? span / length : float2(1.0, 0.0);
    const float2 normal = float2(-direction.y, direction.x);
    const float inked = kGhostLinePass ? style.ghostLine : style.crease;
    const float width = max((kind & kInked) != 0 ? inked : 0.0,
                            (kind & kHighlighted) != 0 ? style.highlight : 0.0);
    const float halfWidth = 0.5 * width + 1.0; // a pixel more, for the soft edge

    // Arc length in pixels, so dashes keep their size and run on across segments.
    const float scale = metal::length(occurrence.rows[0].xyz);
    const float alongA = segment.along * scale;
    const float alongB = alongA + distance(a, b);

    v.kind = kind;
    for (uint k = 0; k < 4; ++k) {
        const float t = float(k >> 1);
        const float side = (k & 1) != 0 ? 1.0 : -1.0;
        float2 s = mix(screenA, screenB, t) + direction * (t * 2.0 - 1.0) * halfWidth * 0.5 +
                   normal * side * halfWidth;
        // The composite samples at pixel + wobble, so a boundary appears shifted by minus the
        // wobble there; lines move with it.
        const float2 pixel = float2(s.x + halfViewport.x, halfViewport.y - s.y);
        const float2 wobble = wobbleOffset(pixel, style);
        s -= float2(wobble.x, -wobble.y);
        const float4 clip = mix(clipA, clipB, t);
        v.position = float4(s / halfViewport * clip.w, clip.z, clip.w);
        v.across = side * halfWidth;
        v.along = mix(alongA / pixelWorld(a), alongB / pixelWorld(b), t);
        if (kSection) {
            const float3 at = mix(a, b, t);
            for (uint p = 0; p < kMaxSectionPlanes; ++p) {
                const float4 plane = frame.sectionPlanes[p];
                v.clip[p] = p < frame.sectionPlaneCount ? -(dot(plane.xyz, at) + plane.w) : 1.0;
            }
        }
        out.set_vertex(first + k, v);
    }
}

fragment InkOutput edgeFragment(EdgeFragmentIn in [[stage_in]],
                                constant StyleUniforms& style [[buffer(kBufferStyle)]]) {
    const float distance = abs(in.across);
    const auto cover = [&](float width) {
        return 1.0 - smoothstep(0.5 * width - 0.5, 0.5 * width + 0.5, distance);
    };
    // Ghost lines are dashed, 4 on and 3 off in points: the drafting sign for what is behind.
    const float period = 7.0 * style.pixelsPerPoint;
    const float phase = fract(in.along / period);
    const float soft = fwidth(in.along / period);
    float4 ink = float4(0.0);
    if (kGhostLinePass) {
        ink.g = (in.kind & kInked) != 0
                    ? cover(style.ghostLine) *
                          (1.0 - smoothstep(4.0 / 7.0 - soft, 4.0 / 7.0 + soft, phase))
                    : 0.0;
    } else {
        ink.r = (in.kind & kInked) != 0 ? cover(style.crease) : 0.0;
        ink.b = (in.kind & kHighlighted) != 0 ? cover(style.highlight) : 0.0;
    }
    return {ink};
}
