// The surface pipeline: object-stage culling, wide mesh shading and shading.
//
// Rules that keep the Apple GPU's hidden surface removal working: no discard and no depth output on
// this path, and varyings stay small. See Docs/PLAN.md.
#include "geometry.h"
#include "shared/gpu_types.h"
#include <metal_stdlib>
using namespace metal;

// The ghost pass reuses every stage, with this constant choosing which faces it keeps.
constant bool kGhostPass [[function_constant(0)]];
// Studio light with metal and roughness, rather than three flat bands.
constant bool kPhysical [[function_constant(1)]];
// The cast shadow: every solid face flattened onto the ground along the light.
constant bool kShadowPass [[function_constant(3)]];

// Where a point's shadow lands on the ground.
static inline float3 shadowOf(float3 p, constant FrameUniforms& frame) {
    const float height = p.z - frame.ground;
    return p + frame.lightTravel * (height / max(-frame.lightTravel.z, 1e-3));
}

struct SurfaceVertex {
    float4 position [[position]];
    float3 normal;
    uint id [[flat]];
    uint flags [[flat]]; // state flags in the low byte, the body's tag in the next, its look above
};

struct SurfaceOutput {
    float4 color [[color(kAttachmentColor)]];
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
    uint2 tags [[color(kAttachmentTags)]]; // the pipeline's write mask picks solid or ghost
};

// Indices only, to keep the payload small: in a run batch they are meshlets of `occurrence`, and
// in a packed batch they index the MeshletRefs.
struct Payload {
    uint visible[kBatchMeshlets];
    uint occurrence; // kNoOccurrence for a packed batch
};

constant uint kNoOccurrence = 0xFFFFFFFF;

static inline bool isVisible(const device Meshlet& m, const device GpuDefinition& definition,
                             const device GpuOccurrence& occurrence,
                             constant FrameUniforms& frame) {
    const float3 center =
        boundsCenter(definition, occurrence, ushort3(m.centerX, m.centerY, m.centerZ));
    const float radius = float(m.radius) * definition.boundsStep;
    if (kShadowPass) {
        // Flattened along a light 75 degrees up, a sphere stretches by under 4%.
        return sphereInView(shadowOf(center, frame), radius * 1.05, frame);
    }
    if (!sphereInView(center, radius, frame)) {
        return false;
    }
    const float3 offset = center - frame.eye;

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
    if (kShadowPass) {
        return (flags & kStateGhost) == 0; // ghosts cast no shadow
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
                   const device MeshletRef* refs [[buffer(kBufferMeshletRefs)]],
                   uint group [[threadgroup_position_in_grid]]) {
    const device Batch& batch = batches[drawBase + group];
    uint count = 0;
    if (batch.kind == kBatchRun) {
        if (passKeeps(states[batch.occurrence].flags)) {
            const device GpuOccurrence& occurrence = occurrences[batch.occurrence];
            const device GpuDefinition& definition = definitions[occurrence.definition];
            for (uint i = 0; i < batch.count; ++i) {
                const uint index = batch.first + i;
                if (isVisible(definition.meshlets[index], definition, occurrence, frame)) {
                    payload.visible[count++] = index;
                }
            }
        }
    } else {
        for (uint i = 0; i < batch.count; ++i) {
            const MeshletRef ref = refs[batch.first + i];
            if (!passKeeps(states[ref.occurrence].flags)) {
                continue;
            }
            const device GpuOccurrence& occurrence = occurrences[ref.occurrence];
            const device GpuDefinition& definition = definitions[occurrence.definition];
            if (isVisible(definition.meshlets[ref.meshlet], definition, occurrence, frame)) {
                payload.visible[count++] = batch.first + i;
            }
        }
    }
    payload.occurrence = batch.kind == kBatchRun ? batch.occurrence : kNoOccurrence;
    grid.set_threadgroups_per_grid(uint3(count, 1, 1));
}

using SurfaceMesh = metal::mesh<SurfaceVertex, void, kMeshletMaxVertices, kMeshletMaxTriangles,
                                metal::topology::triangle>;

static inline SurfaceVertex decodeVertex(const device Meshlet& m,
                                         const device GpuDefinition& definition,
                                         const device GpuOccurrence& occurrence,
                                         OccurrenceState state, const device uchar* faceStates,
                                         uint occurrenceIndex, uint look,
                                         constant FrameUniforms& frame, uint index) {
    const uint vertexIndex = m.vertexOffset + index;
    const device PackedVertex& v = definition.vertices[vertexIndex];
    const float3 world = transformPoint(occurrence, decodePosition(definition, vertexIndex));
    const float3 normal = transformDirection(occurrence, decodeOctahedral(short2(v.octX, v.octY)));

    const uint face = m.faceBase + uint(v.localFace);
    const uint flags = faceFlags(state, faceStates, face);
    SurfaceVertex out;
    out.position = frame.viewProjection * float4(world, 1.0);
    out.normal = frame.viewRotation * normal;
    out.id = occurrence.idBase + face + 1;
    // Neighbouring bodies have different tags, unless their indices differ by a multiple of 255.
    const uint tag = 1 + occurrenceIndex % 255;
    out.flags = flags | tag << 8 | look << 16;
    // Faces never share vertices, so moving all of a face's vertices to one point outside the view
    // removes exactly that face, with no discard and no per-primitive data.
    const bool ghost = (flags & kStateGhost) != 0;
    if ((flags & kStateHidden) != 0 || ghost != kGhostPass) {
        out.position = float4(0.0, 0.0, 2.0, 1.0);
    } else if (kShadowPass) {
        // Held at mid depth, since the flattened model can reach outside the depth range.
        const float4 clip = frame.viewProjection * float4(shadowOf(world, frame), 1.0);
        out.position = float4(clip.xy, 0.5 * clip.w, clip.w);
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
                 const device MeshletRef* refs [[buffer(kBufferMeshletRefs)]],
                 const device ushort* looks [[buffer(kBufferLooks)]],
                 uint index [[thread_index_in_threadgroup]],
                 uint group [[threadgroup_position_in_grid]]) {
    const MeshletRef ref = payload.occurrence != kNoOccurrence
                               ? MeshletRef{payload.occurrence, payload.visible[group]}
                               : refs[payload.visible[group]];
    const device GpuOccurrence& occurrence = occurrences[ref.occurrence];
    const device GpuDefinition& definition = definitions[occurrence.definition];
    const device Meshlet& m = definition.meshlets[ref.meshlet];
    const OccurrenceState state = states[ref.occurrence];

    if (index == 0) {
        out.set_primitive_count(m.triangleCount);
    }
    if (index < m.vertexCount) {
        out.set_vertex(index,
                       decodeVertex(m, definition, occurrence, state, faceStates, ref.occurrence,
                                    uint(looks[ref.occurrence]), frame, index));
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

// ---- Flat bands, after Colorfigures --------------------------------------------------------

// Three flat tones with a soft step between them. The step widens with the normal's rate of
// change, so curved parts stay smooth and flat faces keep crisp bands.
static inline float3 band3(float d, float hi, float lo, float soft, float3 light, float3 mid,
                           float3 dark) {
    const float a = smoothstep(hi - soft, hi + soft, d);
    const float b = smoothstep(lo - soft, lo + soft, d);
    return a > 0.0 ? mix(mid, light, a) : mix(dark, mid, b);
}

// ---- The studio, after step_viewer ----------------------------------------------------------
// Fixed to the camera, in view space with y up: a soft sky over a darker floor, light walls to
// either side, a key box above and to the right, a weaker fill to the left, a bright horizon strip
// and tall strip lights, which together make curved metal read as metal.

constant float3 kKey = float3(0.4537, 0.7059, 0.5440);
constant float3 kFill = float3(-0.7526, 0.1003, 0.6508);

// smoothstep that also runs downhill, when edge0 > edge1.
static inline float ramp(float edge0, float edge1, float x) {
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

static float3 studio(float3 d, float rough, constant StyleUniforms& s) {
    const float spread = mix(0.05, 0.55, rough);
    const float edge = mix(1.0, 0.3, s.sharpness);
    float3 c = mix(s.floor, s.sky, ramp(-0.35, 0.65, d.y));
    c += s.fill * ramp(0.2, 0.9, abs(d.x)) * (d.x > 0.0 ? 0.8 : 0.6);
    c *= 1.0 - 0.5 * ramp(0.15, 0.95, d.z) * (1.0 - abs(d.x));
    const float key =
        ramp(1.0 - spread * (0.9 + 0.8 * edge), 1.0 - spread * (0.9 - 0.75 * edge), dot(d, kKey));
    const float fill =
        ramp(1.0 - 2.2 * spread, 1.0 - 2.2 * spread * (1.0 - edge * 0.9), dot(d, kFill));
    const float q = (d.y - 0.16) / (0.06 + 0.32 * rough);
    const float strip = exp(-q * q);
    const float stripWidth = 0.07 + 0.3 * rough;
    const float stripEdge = stripWidth * mix(0.9, 0.25, s.sharpness);
    const float tall = ramp(0.98, 0.8, abs(d.y));
    const float right =
        ramp(stripWidth + stripEdge, stripWidth - stripEdge, abs(d.x - 0.62)) * tall;
    const float left = ramp(stripWidth + stripEdge, stripWidth - stripEdge, abs(d.x + 0.7)) * tall;
    const float3 white = float3(1.0, 0.99, 0.97);
    return c + s.key * key + s.fill * fill + s.band * strip +
           white * (s.strips.x * right + s.strips.y * left);
}

// Linear light to display values: exposure, then a gentle shoulder or a filmic curve.
static float3 tone(float3 c, constant StyleUniforms& s) {
    c *= s.exposure;
    const float3 shoulder = c / (1.0 + 0.22 * c);
    const float3 film = saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    return pow(max(mix(shoulder, film, s.contrast), 0.0), 1.0 / 2.2);
}

static float3 irradiance(float3 n, constant StyleUniforms& s) {
    const float3 ambient = mix(s.floor, s.sky, 0.5 + 0.5 * n.y);
    const float wrap = max(0.0, (dot(n, kKey) + 0.35) / 1.35);
    return 0.75 * ambient + 0.4 * s.key * wrap * wrap + 0.15 * s.fill * max(dot(n, kFill), 0.0);
}

// A surface of base colour, metalness and roughness under the studio, in display values.
static float3 lit(float3 n, float3 v, float3 baseColor, float metal, float rough,
                  constant StyleUniforms& s) {
    const float3 base = pow(baseColor, 2.2);
    const float nv = saturate(dot(n, v));
    const float3 f0 = mix(float3(0.04), base, metal);
    const float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0) * (1.0 - 0.75 * rough);
    const float3 specular = studio(reflect(-v, n), rough, s) * fresnel;
    // Metals keep a little body colour, as a satin finish does.
    const float3 diffuse = base * irradiance(n, s) * mix(1.0, 0.4, metal);
    return tone(diffuse + specular, s);
}

// The direction toward the eye at a pixel, in view space.
static inline float3 toEye(float2 pixel, constant FrameUniforms& frame) {
    if (frame.perspective == 0) {
        return float3(0.0, 0.0, 1.0);
    }
    const float2 ndc =
        float2(pixel.x / frame.viewport.x * 2.0 - 1.0, 1.0 - pixel.y / frame.viewport.y * 2.0);
    return normalize(float3(-ndc.x * frame.tanHalfX, -ndc.y * frame.tanHalfY, 1.0));
}

static inline float3 texelColor(uint texel) {
    return unpack_unorm4x8_to_float(texel).rgb;
}

fragment SurfaceOutput surfaceFragment(SurfaceVertex in [[stage_in]],
                                       bool frontFacing [[front_facing]],
                                       constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                       constant StyleUniforms& style [[buffer(kBufferStyle)]],
                                       const device uint* palette [[buffer(kBufferPalette)]]) {
    const uint flags = in.flags & 0xFFu;
    const uint tag = (in.flags >> 8) & 0xFFu;
    const uint look = in.flags >> 16;
    const bool selected = (flags & kStateSelected) != 0;
    const uint3 texels =
        selected ? style.selection.xyz
                 : uint3(palette[look * 3], palette[look * 3 + 1], palette[look * 3 + 2]);
    const float3 t0 = texelColor(texels.x), t1 = texelColor(texels.y), t2 = texelColor(texels.z);
    const float3 n = normalize(in.normal);

    // The inside of an open surface, seen from behind, is a little darker than its outside.
    float3 color;
    if (kPhysical) {
        const float3 v = toEye(in.position.xy, frame);
        color = frontFacing ? lit(n, v, t0, t1.x, t1.y, style)
                            : 0.82 * lit(-n, v, t0, t1.x, t1.y, style);
    } else {
        // Derivatives are taken before choosing a side, where every pixel of a quad runs them.
        const float outside = dot(n, style.lightExterior);
        const float inside = dot(-n, style.lightInterior);
        const float softOutside = clamp(1.5 * fwidth(outside), 0.02, 0.18);
        const float softInside = clamp(1.5 * fwidth(inside), 0.02, 0.18);
        color = frontFacing ? band3(outside, -0.15, -0.45, softOutside, t0, t1, t2)
                            : band3(inside, 0.6, 0.05, softInside, t0, t1, t2);
    }

    const bool hovered = in.id >= frame.hoverFirst && in.id < frame.hoverEnd;
    if (kGhostPass) {
        // A faint skin over what lies behind, or none at all for a ghost drawn as lines only.
        // Hovered with the ghost tool held, it shows enough to be clicked back.
        color = mix(color, style.sheetTop, 0.45);
        float alpha = (style.ghostStyle & kGhostShell) != 0 ? style.ghostAlpha : 0.0;
        if (hovered) {
            color = mix(color, style.hover, style.hoverAmount);
            alpha = max(alpha, style.ghostAlpha) * 1.6;
        }
        return {float4(color, alpha), in.id, in.position.z, uint2(tag)};
    }
    if (hovered) {
        // On a selection hover lifts only half as much, so the selection stays readable.
        color = frame.hoverHides != 0
                    ? mix(color, style.hideTint, 0.6)
                    : mix(color, style.hover, style.hoverAmount * (selected ? 0.5 : 1.0));
    }
    return {float4(color, 1.0), in.id, in.position.z, uint2(tag)};
}

// The shadow mask: coverage only.
fragment float4 shadowFragment(SurfaceVertex in [[stage_in]]) {
    return float4(1.0);
}

// Copies each pixel's ID and depth to memory the CPU reads directly, so hover and pivots need no
// GPU round trip, and its tags for the outlines. The attachments themselves stay memoryless.
// Sample 0 is enough for both.
struct PickTile {
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
    uint2 tags [[color(kAttachmentTags)]];
};

kernel void resolvePick(imageblock<PickTile, imageblock_layout_implicit> tile,
                        texture2d<uint, access::write> ids [[texture(kTexturePickIds)]],
                        texture2d<float, access::write> depths [[texture(kTexturePickDepths)]],
                        texture2d<uint, access::write> tags [[texture(kTextureTags)]],
                        ushort2 pixel [[thread_position_in_threadgroup]],
                        ushort2 tileIndex [[threadgroup_position_in_grid]]) {
    const uint2 position = uint2(tileIndex) * uint(kTileSize) + uint2(pixel);
    if (position.x >= ids.get_width() || position.y >= ids.get_height()) {
        return;
    }
    const PickTile sample = tile.read(pixel, 0, imageblock_data_rate::sample);
    ids.write(uint4(sample.id), position);
    depths.write(float4(sample.depth), position);
    tags.write(uint4(sample.tags, 0, 0), position);
}
