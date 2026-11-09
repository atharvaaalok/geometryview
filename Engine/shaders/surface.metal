// The surface pipeline: object-stage culling, wide mesh shading and shading.
//
// Rules that keep the Apple GPU's hidden surface removal working: no discard and no depth output on
// this path, and varyings stay small. See Docs/PLAN.md.
#include "geometry.h"
#include "shared/gpu_types.h"
#include "sheet.h"
#include <metal_stdlib>
using namespace metal;

// The ghost pass reuses every stage, with this constant choosing which faces it keeps.
constant bool kGhostPass [[function_constant(0)]];
// Studio light with metal and roughness, rather than three flat bands.
constant bool kPhysical [[function_constant(1)]];
// The cast shadow: every solid face flattened onto the ground along the light.
constant bool kShadowPass [[function_constant(3)]];
// Section planes are on: geometry is clipped, and back faces seen through a cut are caps.
constant bool kSection [[function_constant(4)]];

// Classifying the shell: ids name occurrences rather than faces, and the peel pass keeps only the
// nearest surface of another part behind the one in front.
constant bool kOccurrenceIds [[function_constant(5)]];
constant bool kPeel [[function_constant(6)]];
// Which see-through layer this is. See ShellLayer.
constant uint kShellLayer [[function_constant(7)]];

// Set in a vertex's flags when its definition is not closed, so a cut through it has no cap.
constant uint kFlagOpen = 0x80;
// Set when the occurrence is part of the see-through shell.
constant uint kFlagShell = 0x08;

// Where a point's shadow lands on the ground.
static inline float3 shadowOf(float3 p, constant FrameUniforms& frame) {
    const float height = p.z - frame.ground;
    return p + frame.lightTravel * (height / max(-frame.lightTravel.z, 1e-3));
}

struct SurfaceVertex {
    float4 position [[position]];
    float clip [[clip_distance, function_constant(kSection)]][kMaxSectionPlanes];
    float3 normal;
    uint id [[flat]];
    uint flags [[flat]]; // state flags in the low byte, the body's tag in the next, its look above
};

// What the fragment stage sees: the vertex without its clip distances.
struct SurfaceFragmentIn {
    float4 position [[position]];
    float3 normal;
    uint id [[flat]];
    uint flags [[flat]];
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
    if (kSection) {
        // Wholly on the cut side of a plane, it would be clipped away.
        for (uint k = 0; k < frame.sectionPlaneCount; ++k) {
            const float4 plane = frame.sectionPlanes[k];
            if (dot(plane.xyz, center) + plane.w > radius) {
                return false;
            }
        }
    }

    // An open surface shows both sides, so a normal cone proves nothing about it; nor does it
    // where back faces are drawn: as caps through a section, and as the shell's far walls.
    if ((definition.flags & kDefinitionOpen) != 0 || kSection || kShellLayer == kShellBack) {
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
    // Neighbouring bodies have different tags, unless their indices differ by a multiple of 127.
    const uint tag = 1 + occurrenceIndex % kTagValues;
    const uint open = (definition.flags & kDefinitionOpen) != 0 ? kFlagOpen : 0;
    const bool shell = (look & kLookShell) != 0;
    out.flags = flags | open | (shell ? kFlagShell : 0) | tag << 8 | (look & kLookIndex) << 16;
    if (kOccurrenceIds) {
        out.id = occurrenceIndex + 1;
    }
    if (kSection) {
        for (uint k = 0; k < kMaxSectionPlanes; ++k) {
            const float4 plane = frame.sectionPlanes[k];
            out.clip[k] = k < frame.sectionPlaneCount ? -(dot(plane.xyz, world) + plane.w) : 1.0;
        }
    }
    // Faces never share vertices, so moving all of a face's vertices to one point outside the view
    // removes exactly that face, with no discard and no per-primitive data.
    const bool ghost = (flags & kStateGhost) != 0;
    // While see-through fades the shell, opaque passes leave it out and the shell layers draw it.
    const bool fading = frame.seeThrough > 0.0;
    bool dropped = (flags & kStateHidden) != 0 || ghost != kGhostPass;
    if (kOccurrenceIds) {
        dropped = dropped || (shell && frame.excludeShells != 0);
    } else if (kShellLayer == kShellNone) {
        dropped = dropped || (shell && fading && !kGhostPass && !kShadowPass);
    } else {
        dropped = dropped || !shell || !fading;
    }
    if (dropped) {
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
    // Metals keep a little body colour, as a satin finish does, and less when polished.
    const float3 diffuse = base * irradiance(n, s) * mix(1.0, s.metalDiffuse, metal);
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

// ---- Section caps, after step_viewer ---------------------------------------------------------
// A back face seen through a cut is exactly where the view ray crossed the plane inside a closed
// part, so it is shaded as the cut surface, hatched in the plane's own coordinates.

// The view ray through a pixel, in world space.
static inline void viewRay(float2 pixel, constant FrameUniforms& frame, thread float3& origin,
                           thread float3& direction) {
    const float2 ndc =
        float2(pixel.x / frame.viewport.x * 2.0 - 1.0, 1.0 - pixel.y / frame.viewport.y * 2.0);
    if (frame.perspective != 0) {
        origin = frame.eye;
        direction = normalize(frame.forward + frame.right * ndc.x * frame.tanHalfX +
                              frame.up * ndc.y * frame.tanHalfY);
    } else {
        origin =
            frame.eye + frame.right * ndc.x * frame.halfWidth + frame.up * ndc.y * frame.halfHeight;
        direction = frame.forward;
    }
}

// Coverage of one family of hatch lines, `q` counting lines. Lines crowded closer than a few
// pixels fade out rather than turn to moiré.
static inline float hatchLines(float q, float pixelsPerPoint) {
    const float perPixel = max(length(float2(dfdx(q), dfdy(q))), 1e-6);
    const float pixels = abs(fract(q) - 0.5) / perPixel;
    const float crowding = smoothstep(3.5 * pixelsPerPoint, 5.5 * pixelsPerPoint, 1.0 / perPixel);
    return crowding * saturate(0.4 * pixelsPerPoint + 0.5 - pixels);
}

// Hatch coverage at a pixel, on the plane the view ray entered the kept region through: 45 degree
// lines about 0.8 points wide. Each body's tag varies their direction and spacing, and crosses
// some, so cut parts side by side differ.
static float capHatch(float2 pixel, uint tag, constant FrameUniforms& frame,
                      constant StyleUniforms& style) {
    float3 origin, direction;
    viewRay(pixel, frame, origin, direction);
    uint entry = 0;
    float entryT = -INFINITY;
    for (uint k = 0; k < frame.sectionPlaneCount; ++k) {
        const float4 plane = frame.sectionPlanes[k];
        const float denominator = dot(plane.xyz, direction);
        if (denominator < 0.0) { // from the cut side into the kept side
            const float t = -(dot(plane.xyz, origin) + plane.w) / denominator;
            if (t > entryT) {
                entryT = t;
                entry = k;
            }
        }
    }
    const float3 normal = frame.sectionPlanes[entry].xyz;
    const float t = isfinite(entryT) ? entryT : 0.0;
    const float3 world = origin + direction * t;
    const float3 u =
        normalize(cross(normal, abs(normal.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0)));
    const float3 v = cross(normal, u);
    const float depth =
        frame.perspective != 0 ? max(dot(world - frame.eye, frame.forward), 1e-6) : 1.0;
    const float pixelWorld = frame.worldPerPixel * depth;
    const float sense = (tag & 1) != 0 ? 1.0 : -1.0;
    const float spacing =
        (6.0 + 2.0 * float(tag % 3)) * style.pixelsPerPoint * pixelWorld * 1.41421356;
    const float a = dot(world, u), b = dot(world, v);
    const float lines = hatchLines((a + sense * b) / spacing, style.pixelsPerPoint);
    const float crossing = hatchLines((a - sense * b) / spacing, style.pixelsPerPoint);
    return max(lines, tag % 4 == 0 ? crossing : 0.0);
}

fragment SurfaceOutput surfaceFragment(SurfaceFragmentIn in [[stage_in]],
                                       bool frontFacing [[front_facing]],
                                       constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                       constant StyleUniforms& style [[buffer(kBufferStyle)]],
                                       const device uint* palette [[buffer(kBufferPalette)]],
                                       texture2d<uint> frontIds
                                       [[texture(kTexturePickIds), function_constant(kPeel)]],
                                       texture2d<float> frontDepths
                                       [[texture(kTexturePickDepths), function_constant(kPeel)]]) {
    if (kPeel) {
        // Only what lies behind the front surface, and belongs to another part, is kept.
        const uint2 p = uint2(in.position.xy);
        const float front = frontDepths.read(p).r;
        if (frontIds.read(p).r == in.id || in.position.z >= front - (1e-5 * front + 1e-7)) {
            discard_fragment();
        }
    }
    const uint flags = in.flags & 0x77u; // state, without the shell and open bits
    uint tag = (in.flags >> 8) & 0xFFu;
    // Computed before any branch, since hatch width comes from screen derivatives.
    const float hatch = kSection ? capHatch(in.position.xy, tag, frame, style) : 0.0;
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

    // A cut through a closed part: the cap, hatched, and tagged so outlines mark the cut.
    if (kSection && !frontFacing && (in.flags & kFlagOpen) == 0 && frame.sectionPlaneCount > 0) {
        const float3 fill = kPhysical ? t2 : t0;
        const float3 line = kPhysical ? mix(t2 * 0.6, style.ink, max(style.capInk, 0.15))
                                      : mix(t2, style.ink, max(style.capInk, 0.3));
        color = mix(fill, line, hatch);
        tag |= kTagCap;
    }

    const bool hovered = in.id >= frame.hoverFirst && in.id < frame.hoverEnd;
    if (kShellLayer == kShellBack) {
        // The shell's far walls lie under everything, over the sheet they are mixed into.
        const float3 wall = mix(color, style.sheetTop, 0.55);
        return {float4(mix(sheetColor(in.position.xy, frame, style), wall, 0.5 * frame.seeThrough),
                       1.0),
                in.id, in.position.z, uint2(0)};
    }
    if (kShellLayer == kShellFront) {
        if (hovered) {
            color = mix(color, style.hover, style.hoverAmount * (selected ? 0.5 : 1.0));
        }
        return {float4(color, 1.0 - 0.8 * frame.seeThrough), in.id, in.position.z,
                uint2(0, kTagShell | tag)};
    }
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
fragment float4 shadowFragment(SurfaceFragmentIn in [[stage_in]]) {
    return float4(1.0);
}

// Copies each pixel's ID and depth to memory the CPU reads directly, so hover and pivots need no
// GPU round trip, and its tags for the outlines. The attachments themselves stay memoryless.
// Sample 0 is enough for both.
struct ResolveTile {
    half4 color [[color(kAttachmentColor)]];
    uint id [[color(kAttachmentId)]];
    float depth [[color(kAttachmentDepth)]];
    half4 ink [[color(kAttachmentInk)]];
    uint2 tags [[color(kAttachmentTags)]];
};

// Also inks every sample from the ink attachment. Doing it here, in the pass's one tile dispatch,
// matters: any mid-pass step that reads the tiles after a million meshlets cost from 5 to over
// 100 ms. Creases and highlights were drawn before ghosts, so a ghost's skin in front, known from
// its tag, dims them; ghosts' own dashed lines were drawn after and are not dimmed.
kernel void resolveTile(imageblock<ResolveTile, imageblock_layout_implicit> tile,
                        texture2d<uint, access::write> ids [[texture(kTexturePickIds)]],
                        texture2d<float, access::write> depths [[texture(kTexturePickDepths)]],
                        texture2d<uint, access::write> tags [[texture(kTextureTags)]],
                        constant StyleUniforms& style [[buffer(kBufferStyle)]],
                        constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                        ushort2 pixel [[thread_position_in_threadgroup]],
                        ushort2 tileIndex [[threadgroup_position_in_grid]]) {
    const half ink = half(style.inkAlpha);
    const half skin = (style.ghostStyle & kGhostShell) != 0 ? half(style.ghostAlpha) : 0.0h;
    for (ushort s = 0; s < tile.get_num_samples(); ++s) {
        ResolveTile t = tile.read(pixel, s, imageblock_data_rate::sample);
        half solid = 1.0h - (1.0h - t.ink.r * ink) * (1.0h - t.ink.b);
        if ((t.tags.g & kTagShell) != 0) {
            solid *= half(0.8 * frame.seeThrough); // what shows through the faded shell
        } else if (t.tags.g != 0) {
            solid *= 1.0h - skin;
        }
        const half ghost = t.ink.g * half(style.ghostLineAlpha) * ink;
        const half cover = 1.0h - (1.0h - solid) * (1.0h - ghost);
        if (cover > 0.0h) {
            t.color.rgb = mix(t.color.rgb, half3(style.ink), cover);
            tile.write(t, pixel, s, imageblock_data_rate::sample);
        }
    }

    const uint2 position = uint2(tileIndex) * uint(kTileSize) + uint2(pixel);
    if (position.x >= ids.get_width() || position.y >= ids.get_height()) {
        return;
    }
    const ResolveTile sample = tile.read(pixel, 0, imageblock_data_rate::sample);
    ids.write(uint4(sample.id), position);
    depths.write(float4(sample.depth), position);
    tags.write(uint4(sample.tags, 0, 0), position);
}
