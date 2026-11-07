// The spike's WGSL: cull and compact, the visibility pass, and the composite that rebuilds each
// pixel's surface from its visibility id.
//
// The cull pass tests each meshlet instance and writes the visible ones as 16-triangle clusters,
// with one indirect draw's worth of vertices for closed surfaces and one for open ones. The
// visibility pass pulls its vertices from the cache's own records: 48 invocations per cluster, a
// triangle each three, with no index buffer. A pixel stores its cluster and triangle, and the
// composite finds the triangle again, interpolates its normal and shades.
#pragma once

namespace geo::spike {

inline constexpr const char* kShaderSource = R"wgsl(
struct Frame {
    viewProjection: mat4x4f,
    viewRotation: mat4x4f,
    eye: vec3f, perspective: u32,
    right: vec3f, halfWidth: f32,
    up: vec3f, halfHeight: f32,
    forward: vec3f, tanHalfX: f32,
    viewport: vec2f, tanHalfY: f32, secHalfX: f32,
    secHalfY: f32, instanceCount: u32, openClusterStart: u32, edgeInstanceCount: u32,
    lightTravel: vec3f, ground: f32,
    worldPerPixel: f32, pixelsPerPoint: f32, depthNear: f32, depthFar: f32,
    aoRadius: f32, aoSamples: u32, features: u32, openIndexStart: u32,
}

struct Definition {
    meshletBase: u32, vertexBase: u32, highBitsBase: u32, triangleBase: u32,
    boundsMin: vec3f, boundsStep: f32,
    decodeScale: vec3f, flags: u32,
    boundsScale: vec3f, vertexCount: u32,
    edgeChunkBase: u32, edgeSegmentBase: u32, faceCount: u32, pad0: u32,
}

struct Occurrence { row0: vec4f, row1: vec4f, row2: vec4f, definition: u32, idBase: u32,
                    pad0: u32, pad1: u32 }

// The cache's 32-byte meshlet, read as words.
// a: centre x | y << 16, centre z | radius << 16, vertex offset, triangle offset
// b: face base, vertices | triangles << 8 | cone x << 16 | cone y << 24, cone z | cutoff << 8, -
struct Meshlet { a: vec4u, b: vec4u }

struct DrawArgs { vertexCount: atomic<u32>, instanceCount: u32, firstVertex: u32,
                  firstInstance: u32 }

const kDefinitionWide = 1u;
const kDefinitionOpen = 2u;
const kClusterTriangles = 16u;
const kClusterVertices = 48u;

@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var<storage, read> definitions: array<Definition>;
@group(0) @binding(2) var<storage, read> occurrences: array<Occurrence>;
@group(0) @binding(3) var<storage, read> meshlets: array<Meshlet>;
@group(0) @binding(4) var<storage, read> instances: array<vec2u>;
@group(0) @binding(5) var<storage, read> vertices: array<u32>;
@group(0) @binding(6) var<storage, read> highBits: array<u32>;
@group(0) @binding(7) var<storage, read> triangles: array<u32>;
@group(0) @binding(8) var<storage, read> clusters: array<u32>;
@group(0) @binding(9) var<storage, read_write> clustersOut: array<u32>;
@group(0) @binding(10) var<storage, read_write> args: array<DrawArgs, 2>;
@group(0) @binding(11) var visTexture: texture_2d<u32>;

// The indexed mode: visible meshlets get slots, and the cull pass writes their triangles' indices,
// so the hardware reuses a vertex across the triangles that share it.
struct IndexedArgs { indexCount: atomic<u32>, instanceCount: u32, firstIndex: u32, baseVertex: i32,
                     firstInstance: u32 }
@group(0) @binding(12) var<storage, read_write> slotsOut: array<u32>;
@group(0) @binding(13) var<storage, read_write> indicesOut: array<u32>;
// The two draws' arguments, then the slot counters, in one buffer to stay within eight storage
// buffers per stage.
struct CullCounters { draws: array<IndexedArgs, 2>, slots: array<atomic<u32>, 2> }
@group(0) @binding(14) var<storage, read_write> indexedArgs: CullCounters;
@group(0) @binding(15) var<storage, read> slots: array<u32>;
@group(0) @binding(16) var surfaceTexture: texture_2d<u32>;
@group(0) @binding(17) var normalTexture: texture_2d<f32>;

// Feature edges: chunks of 32 segments, culled into slots and drawn as screen-space quads.
@group(0) @binding(18) var<storage, read> edgeInstances: array<vec2u>; // occurrence, chunk
@group(0) @binding(19) var<storage, read> edgeChunks: array<vec4u>;
@group(0) @binding(20) var<storage, read> edgeSegments: array<u32>;    // five words each
@group(0) @binding(21) var<storage, read_write> edgeSlotsOut: array<u32>;
@group(0) @binding(22) var<storage, read_write> edgeArgs: array<DrawArgs, 1>;
@group(0) @binding(23) var<storage, read> edgeSlots: array<u32>;
// What the composite reads besides the surface.
@group(0) @binding(28) var depthTexture: texture_depth_2d;
@group(0) @binding(29) var inkTexture: texture_2d<f32>;
@group(0) @binding(30) var occlusionTexture: texture_2d<f32>;
@group(0) @binding(31) var shadowTexture: texture_2d<f32>;
// Which body a pixel shows, as a byte that differs between neighbours: what outlines search.
@group(0) @binding(32) var tagTexture: texture_2d<u32>;

const kFeatureEdges = 1u;
const kFeatureShadow = 2u;
const kFeatureOcclusion = 4u;

// The cast shadow reuses the indexed cull and vertex stages: with this set, a meshlet is kept if
// its shadow is in view, and vertices land where their shadow falls on the ground.
override kShadow: bool = false;

// Where a point's shadow lands on the ground.
fn shadowOf(p: vec3f) -> vec3f {
    let height = p.z - frame.ground;
    return p + frame.lightTravel * (height / max(-frame.lightTravel.z, 1e-3));
}

fn transformPoint(o: Occurrence, p: vec3f) -> vec3f {
    let h = vec4f(p, 1.0);
    return vec3f(dot(o.row0, h), dot(o.row1, h), dot(o.row2, h));
}

fn transformDirection(o: Occurrence, v: vec3f) -> vec3f {
    return vec3f(dot(o.row0.xyz, v), dot(o.row1.xyz, v), dot(o.row2.xyz, v));
}

// Integers first, so a grid point decodes to the same float wherever it is read.
fn decodePosition(d: Definition, index: u32) -> vec3f {
    let base = (d.vertexBase + index) * 3u;
    let w0 = vertices[base];
    let w1 = vertices[base + 1u];
    var grid = vec3u(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu);
    if ((d.flags & kDefinitionWide) != 0u) {
        let high = highBits[d.highBitsBase + index];
        grid |= vec3u(high & 0xFFu, (high >> 8u) & 0xFFu, (high >> 16u) & 0xFFu) << vec3u(16u);
    }
    return vec3f(grid) * d.decodeScale + d.boundsMin;
}

fn decodeNormal(d: Definition, index: u32) -> vec3f {
    let w = i32(vertices[(d.vertexBase + index) * 3u + 2u]);
    let e = vec2f(f32(extractBits(w, 0u, 16u)), f32(extractBits(w, 16u, 16u))) / 32767.0;
    var n = vec3f(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n = vec3f((1.0 - abs(n.yx)) * sign(n.xy), n.z);
    }
    return normalize(n);
}

fn localFace(d: Definition, index: u32) -> u32 {
    return vertices[(d.vertexBase + index) * 3u + 1u] >> 16u;
}

// ---- Cull and compact ------------------------------------------------------------------------

fn sphereInView(center: vec3f, radius: f32) -> bool {
    let offset = center - frame.eye;
    let x = abs(dot(offset, frame.right));
    let y = abs(dot(offset, frame.up));
    let depth = dot(offset, frame.forward);
    if (frame.perspective != 0u) {
        return depth >= -radius && x <= depth * frame.tanHalfX + radius * frame.secHalfX &&
               y <= depth * frame.tanHalfY + radius * frame.secHalfY;
    }
    return x <= frame.halfWidth + radius && y <= frame.halfHeight + radius;
}

fn isVisible(m: Meshlet, d: Definition, o: Occurrence) -> bool {
    let grid = vec3f(f32(m.a.x & 0xFFFFu), f32(m.a.x >> 16u), f32(m.a.y & 0xFFFFu));
    let center = transformPoint(o, grid * d.boundsScale + d.boundsMin);
    let radius = f32(m.a.y >> 16u) * d.boundsStep;
    if (kShadow) {
        // Flattened along a light 75 degrees up, a sphere stretches by under 4%.
        return sphereInView(shadowOf(center), radius * 1.05);
    }
    if (!sphereInView(center, radius)) {
        return false;
    }
    // An open surface shows both sides, so its normal cone proves nothing.
    if ((d.flags & kDefinitionOpen) != 0u) {
        return true;
    }
    let cone = vec3f(f32(extractBits(i32(m.b.y), 16u, 8u)), f32(extractBits(i32(m.b.y), 24u, 8u)),
                     f32(extractBits(i32(m.b.z), 0u, 8u)));
    let axis = transformDirection(o, cone / 127.0);
    let cutoff = f32(extractBits(i32(m.b.z), 8u, 8u)) / 127.0;
    if (frame.perspective != 0u) {
        let offset = center - frame.eye;
        return dot(offset, axis) < cutoff * length(offset) + radius;
    }
    return dot(frame.forward, axis) < cutoff;
}

var<workgroup> groupCount: array<atomic<u32>, 2>;
var<workgroup> groupBase: array<u32, 2>;

// One thread per meshlet instance. A workgroup counts its clusters locally and reserves them with
// one global atomic per kind, rather than one per thread.
@compute @workgroup_size(64)
fn cull(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
        @builtin(num_workgroups) groups: vec3u) {
    let index = (group.y * groups.x + group.x) * 64u + lid;
    if (lid < 2u) {
        atomicStore(&groupCount[lid], 0u);
    }
    workgroupBarrier();
    var count = 0u;
    var kind = 0u;
    var offset = 0u;
    if (index < frame.instanceCount) {
        let instance = instances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        let m = meshlets[d.meshletBase + instance.y];
        if (isVisible(m, d, o)) {
            count = (((m.b.y >> 8u) & 0xFFu) + kClusterTriangles - 1u) / kClusterTriangles;
            kind = (d.flags & kDefinitionOpen) >> 1u;
            offset = atomicAdd(&groupCount[kind], count);
        }
    }
    workgroupBarrier();
    if (lid < 2u) {
        let total = atomicLoad(&groupCount[lid]);
        var base = 0u;
        if (total > 0u) {
            base = atomicAdd(&args[lid].vertexCount, total * kClusterVertices) / kClusterVertices;
        }
        groupBase[lid] = base;
    }
    workgroupBarrier();
    let start = groupBase[kind] + offset + select(0u, frame.openClusterStart, kind == 1u);
    for (var k = 0u; k < count; k++) {
        clustersOut[start + k] = (index << 3u) | k;
    }
}

// ---- Visibility --------------------------------------------------------------------------------

struct VisOut {
    @builtin(position) position: vec4f,
    @location(0) @interpolate(flat) vis: u32,
}

@vertex
fn visVertex(@builtin(vertex_index) vertex: u32) -> VisOut {
    var out: VisOut;
    let cluster = vertex / kClusterVertices;
    let t = (vertex % kClusterVertices) / 3u;
    let entry = clusters[cluster];
    let instance = instances[entry >> 3u];
    let triangle = (entry & 7u) * kClusterTriangles + t;
    let o = occurrences[instance.x];
    let d = definitions[o.definition];
    let m = meshlets[d.meshletBase + instance.y];
    let vertexCount = m.b.y & 0xFFu;
    out.vis = cluster * kClusterTriangles + t + 1u;
    if (triangle >= ((m.b.y >> 8u) & 0xFFu)) {
        out.position = vec4f(0.0, 0.0, -1.0, 1.0); // past the last triangle: degenerate
        return out;
    }
    let packed = triangles[d.triangleBase + m.a.w + triangle];
    let local = min(extractBits(packed, (vertex % 3u) * 8u, 8u), vertexCount - 1u);
    let world = transformPoint(o, decodePosition(d, m.a.z + local));
    out.position = frame.viewProjection * vec4f(world, 1.0);
    return out;
}

@fragment
fn visFragment(in: VisOut) -> @location(0) u32 {
    return in.vis;
}

// ---- Indexed mode: cull, expand and draw with vertex reuse ------------------------------------

const kSlotShift = 7u; // an index is slot << 7 | vertex within the meshlet

var<workgroup> slotCount: array<atomic<u32>, 2>;
var<workgroup> indexCount: array<atomic<u32>, 2>;
var<workgroup> slotBase: array<u32, 2>;
var<workgroup> indexBase: array<u32, 2>;
var<workgroup> wantSlot: array<u32, 64>;  // the global slot each thread's meshlet got, or none
var<workgroup> wantIndex: array<u32, 64>; // where its indices start
var<workgroup> wantInstance: array<u32, 64>;

// Each visible meshlet gets a slot, which the vertex stage looks it up by, and exactly as many
// indices as its triangles need, so small parts cost no more than their triangles.
@compute @workgroup_size(64)
fn cullIndexed(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
               @builtin(num_workgroups) groups: vec3u) {
    let index = (group.y * groups.x + group.x) * 64u + lid;
    if (lid < 2u) {
        atomicStore(&slotCount[lid], 0u);
        atomicStore(&indexCount[lid], 0u);
    }
    workgroupBarrier();
    var kind = 0u;
    var local = 0xFFFFFFFFu;
    var localIndex = 0u;
    if (index < frame.instanceCount) {
        let instance = instances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        let m = meshlets[d.meshletBase + instance.y];
        if (isVisible(m, d, o)) {
            // A shadow shows both sides, so open and closed surfaces draw together.
            kind = select((d.flags & kDefinitionOpen) >> 1u, 0u, kShadow);
            local = atomicAdd(&slotCount[kind], 1u);
            localIndex = atomicAdd(&indexCount[kind], ((m.b.y >> 8u) & 0xFFu) * 3u);
        }
    }
    workgroupBarrier();
    if (lid < 2u) {
        let slots = atomicLoad(&slotCount[lid]);
        let indices = atomicLoad(&indexCount[lid]);
        var base = 0u;
        var first = 0u;
        if (slots > 0u) {
            base = atomicAdd(&indexedArgs.slots[lid], slots);
            first = atomicAdd(&indexedArgs.draws[lid].indexCount, indices);
        }
        slotBase[lid] = base + select(0u, frame.openClusterStart, lid == 1u);
        indexBase[lid] = first + select(0u, frame.openIndexStart, lid == 1u);
    }
    workgroupBarrier();
    var slot = 0xFFFFFFFFu;
    if (local != 0xFFFFFFFFu) {
        slot = slotBase[kind] + local;
        slotsOut[slot] = index;
    }
    wantSlot[lid] = slot;
    wantIndex[lid] = indexBase[kind] + localIndex;
    wantInstance[lid] = index;
    workgroupBarrier();

    // Together, write each visible meshlet's indices.
    for (var v = 0u; v < 64u; v++) {
        let slotIndex = wantSlot[v];
        if (slotIndex == 0xFFFFFFFFu) {
            continue;
        }
        let instance = instances[wantInstance[v]];
        let d = definitions[occurrences[instance.x].definition];
        let m = meshlets[d.meshletBase + instance.y];
        let count = ((m.b.y >> 8u) & 0xFFu) * 3u;
        let last = (m.b.y & 0xFFu) - 1u;
        let first = wantIndex[v];
        for (var j = lid; j < count; j += 64u) {
            let packed = triangles[d.triangleBase + m.a.w + j / 3u];
            let vertex = min(extractBits(packed, (j % 3u) * 8u, 8u), last);
            indicesOut[first + j] = (slotIndex << kSlotShift) | vertex;
        }
    }
}

struct SurfaceOut {
    @builtin(position) position: vec4f,
    @location(0) normal: vec3f,
    @location(1) @interpolate(flat) id: vec2u, // pick id, occurrence + 1
}

@vertex
fn visIndexedVertex(@builtin(vertex_index) index: u32) -> SurfaceOut {
    let instance = instances[slots[index >> kSlotShift]];
    let o = occurrences[instance.x];
    let d = definitions[o.definition];
    let m = meshlets[d.meshletBase + instance.y];
    let vertex = m.a.z + min(index & ((1u << kSlotShift) - 1u), (m.b.y & 0xFFu) - 1u);
    var out: SurfaceOut;
    let world = transformPoint(o, decodePosition(d, vertex));
    out.position = frame.viewProjection * vec4f(world, 1.0);
    if (kShadow) {
        // Held at mid depth, since the flattened model can reach outside the depth range.
        let clip = frame.viewProjection * vec4f(shadowOf(world), 1.0);
        out.position = vec4f(clip.xy, 0.5 * clip.w, clip.w);
    }
    out.normal = transformDirection(o, decodeNormal(d, vertex));
    out.id = vec2u(o.idBase + m.b.x + localFace(d, vertex) + 1u, instance.x + 1u);
    return out;
}

// Octahedral, so a view-space normal fits two half floats.
fn encodeNormal(n: vec3f) -> vec2f {
    let p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        return (1.0 - abs(p.yx)) * select(vec2f(-1.0), vec2f(1.0), p >= vec2f(0.0));
    }
    return p;
}

fn decodeViewNormal(e: vec2f) -> vec3f {
    var n = vec3f(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n = vec3f((1.0 - abs(n.yx)) * select(vec2f(-1.0), vec2f(1.0), n.xy >= vec2f(0.0)), n.z);
    }
    return normalize(n);
}

struct SurfaceTargets { @location(0) id: vec2u, @location(1) normal: vec2f, @location(3) tag: u32 }

// Neighbouring bodies get different tags unless their indices differ by a multiple of 255.
fn tagOf(body: u32) -> u32 {
    return 1u + (body - 1u) % 255u;
}

@fragment
fn visIndexedFragment(in: SurfaceOut) -> SurfaceTargets {
    let n = (frame.viewRotation * vec4f(normalize(in.normal), 0.0)).xyz;
    return SurfaceTargets(in.id, encodeNormal(n), tagOf(in.id.y));
}

@fragment
fn shadowFragment() -> @location(0) vec4f {
    return vec4f(1.0);
}

// ---- Feature edges -------------------------------------------------------------------------------

const kChunkSegments = 32u;
const kEdgeVertices = 192u;  // six per segment: two triangles
const kMinChunkPixels = 3.0; // smaller chunks would only blot the part with ink
const kEdgeSmooth = 1u;
const kCrease = 1.25;        // points

var<workgroup> edgeCount: atomic<u32>;
var<workgroup> edgeBase: u32;

@compute @workgroup_size(64)
fn cullEdges(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
             @builtin(num_workgroups) groups: vec3u) {
    let index = (group.y * groups.x + group.x) * 64u + lid;
    if (lid == 0u) {
        atomicStore(&edgeCount, 0u);
    }
    workgroupBarrier();
    var local = 0xFFFFFFFFu;
    if (index < frame.edgeInstanceCount) {
        let instance = edgeInstances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        let c = edgeChunks[d.edgeChunkBase + instance.y];
        let grid = vec3f(f32(c.x & 0xFFFFu), f32(c.x >> 16u), f32(c.y & 0xFFFFu));
        let center = transformPoint(o, grid * d.boundsScale + d.boundsMin);
        let radius = f32(c.y >> 16u) * d.boundsStep;
        if (radius >= kMinChunkPixels * frame.worldPerPixel && sphereInView(center, radius)) {
            local = atomicAdd(&edgeCount, 1u);
        }
    }
    workgroupBarrier();
    if (lid == 0u) {
        let total = atomicLoad(&edgeCount);
        var base = 0u;
        if (total > 0u) {
            base = atomicAdd(&edgeArgs[0].vertexCount, total * kEdgeVertices) / kEdgeVertices;
        }
        edgeBase = base;
    }
    workgroupBarrier();
    if (local != 0xFFFFFFFFu) {
        edgeSlotsOut[edgeBase + local] = index;
    }
}

struct EdgeOut {
    @builtin(position) position: vec4f,
    @location(0) @interpolate(linear) across: f32, // pixels from the line's middle
    @location(1) @interpolate(flat) halfWidth: f32,
}

// Six vertices per segment, a quad around the line on screen, lifted toward the eye so a
// neighbour's slightly different tessellation does not cut it into dashes.
@vertex
fn edgeVertex(@builtin(vertex_index) vertex: u32) -> EdgeOut {
    var out: EdgeOut;
    out.position = vec4f(0.0, 0.0, -1.0, 1.0);
    out.across = 0.0;
    out.halfWidth = 1.0;
    let segment = (vertex / 6u) % kChunkSegments;
    let corner = (0x312210u >> ((vertex % 6u) * 4u)) & 0xFu; // 0 1 2, 2 1 3
    let instance = edgeInstances[edgeSlots[vertex / kEdgeVertices]];
    let o = occurrences[instance.x];
    let d = definitions[o.definition];
    let c = edgeChunks[d.edgeChunkBase + instance.y];
    if (segment >= min(c.w, kChunkSegments)) {
        return out;
    }
    let base = (d.edgeSegmentBase + c.z + segment) * 5u;
    if ((edgeSegments[base + 3u] >> 30u) == kEdgeSmooth) {
        return out; // tangent: inked only around a highlighted face
    }
    let last = max(d.vertexCount, 1u) - 1u;
    let lift = -frame.forward * (2.0 * frame.pixelsPerPoint * frame.worldPerPixel);
    let clipA = frame.viewProjection *
                vec4f(transformPoint(o, decodePosition(d, min(edgeSegments[base], last))) + lift, 1.0);
    let clipB = frame.viewProjection *
                vec4f(transformPoint(o, decodePosition(d, min(edgeSegments[base + 1u], last))) + lift, 1.0);
    let halfViewport = 0.5 * frame.viewport;
    let screenA = clipA.xy / clipA.w * halfViewport;
    let screenB = clipB.xy / clipB.w * halfViewport;
    let span = screenB - screenA;
    let spanLength = length(span);
    let direction = select(vec2f(1.0, 0.0), span / spanLength, spanLength > 1e-4);
    let normal = vec2f(-direction.y, direction.x);
    let halfWidth = 0.5 * kCrease * frame.pixelsPerPoint + 1.0; // a pixel more, for the soft edge
    let t = f32(corner >> 1u);
    let side = select(-1.0, 1.0, (corner & 1u) != 0u);
    let screen = mix(screenA, screenB, t) + direction * (t * 2.0 - 1.0) * halfWidth * 0.5 +
                 normal * side * halfWidth;
    let clip = mix(clipA, clipB, t);
    out.position = vec4f(screen / halfViewport * clip.w, clip.z, clip.w);
    out.across = side * halfWidth;
    out.halfWidth = halfWidth;
    return out;
}

fn lineCover(width: f32, distance: f32) -> f32 {
    return 1.0 - smoothstep(0.5 * width - 0.5, 0.5 * width + 0.5, distance);
}

struct InkOut { @location(2) ink: vec4f }

@fragment
fn edgeFragment(in: EdgeOut) -> InkOut {
    return InkOut(vec4f(lineCover(2.0 * (in.halfWidth - 1.0), abs(in.across))));
}

// ---- Ambient occlusion, at half resolution ----------------------------------------------------

fn viewDepth(depth: f32) -> f32 {
    return frame.depthFar - depth * (frame.depthFar - frame.depthNear);
}

// A pixel's position in view space, looking down -z. Orthographic.
fn viewPosition(p: vec2i, depth: f32) -> vec3f {
    let ndc = vec2f((f32(p.x) + 0.5) / frame.viewport.x * 2.0 - 1.0,
                    1.0 - (f32(p.y) + 0.5) / frame.viewport.y * 2.0);
    return vec3f(ndc * vec2f(frame.halfWidth, frame.halfHeight), -viewDepth(depth));
}

fn depthAt(q: vec2i) -> f32 {
    return textureLoad(depthTexture, clamp(q, vec2i(0), vec2i(frame.viewport) - 1), 0);
}

// The neighbour on an axis that lies on the same surface is the one nearer in depth.
fn tangent(p: vec2i, center: vec3f, axis: vec2i) -> vec3f {
    let da = depthAt(p + axis);
    let db = depthAt(p - axis);
    let a = viewPosition(p + axis, da) - center;
    let b = center - viewPosition(p - axis, db);
    if (da <= 0.0) {
        return b;
    }
    if (db <= 0.0) {
        return a;
    }
    return select(b, a, abs(a.z) < abs(b.z));
}

// How much of the sky above each point nearby geometry hides, from depth alone. The normal comes
// from whichever neighbour on each axis is on the same surface; samples lie on a spiral turned
// per pixel.
@fragment
fn occlusion(in: FullOut) -> @location(0) vec4f {
    let gid = vec2i(in.position.xy);
    let p = gid * 2;
    let d = depthAt(p);
    let tag = textureLoad(tagTexture, clamp(p, vec2i(0), vec2i(frame.viewport) - 1), 0).r;
    if (d <= 0.0 || tag == 0u) {
        return vec4f(1.0, 0.0, 0.0, 0.0);
    }
    let center = viewPosition(p, d);
    var normal = normalize(cross(tangent(p, center, vec2i(1, 0)), tangent(p, center, vec2i(0, -1))));
    if (normal.z < 0.0) {
        normal = -normal;
    }
    let px = frame.pixelsPerPoint;
    let reach = clamp(frame.aoRadius / frame.worldPerPixel, 4.0 * px, 48.0 * px);
    let radius = reach * frame.worldPerPixel;
    let turn = 6.2831853 * fract(52.9829189 * fract(dot(vec2f(gid) + 0.5, vec2f(0.06711056, 0.00583715))));
    var occluded = 0.0;
    for (var i = 0u; i < frame.aoSamples; i++) {
        let t = (f32(i) + 0.5) / f32(frame.aoSamples);
        let angle = turn + f32(i) * 2.3999632;
        let q = p + vec2i(round(vec2f(cos(angle), sin(angle)) * t * reach));
        let dq = depthAt(q);
        if (dq <= 0.0) {
            continue;
        }
        let v = viewPosition(q, dq) - center;
        let distance = length(v);
        let falloff = saturate(1.0 - (distance * distance) / (radius * radius));
        occluded += max(dot(normal, v) / max(distance, 1e-6) - 0.05, 0.0) * falloff;
    }
    // With its view depth, so the composite's blur needs no depth reads of its own.
    return vec4f(pow(saturate(1.0 - 2.6 * occluded / f32(frame.aoSamples)), 1.4), -center.z, 0.0, 0.0);
}

// ---- Composite ---------------------------------------------------------------------------------

struct FullOut { @builtin(position) position: vec4f }

@vertex
fn fullscreen(@builtin(vertex_index) vertex: u32) -> FullOut {
    let uv = vec2f(f32((vertex << 1u) & 2u), f32(vertex & 2u));
    return FullOut(vec4f(uv * 2.0 - 1.0, 0.0, 1.0));
}

struct Surface { normal: vec3f, occurrence: u32, id: u32 }

fn occurrenceOf(vis: u32) -> u32 {
    return instances[clusters[(vis - 1u) / kClusterTriangles] >> 3u].x;
}

// Finds the pixel's triangle again and interpolates its normal, perspective-correct.
fn reconstruct(vis: u32, pixel: vec2f) -> Surface {
    let cluster = (vis - 1u) / kClusterTriangles;
    let entry = clusters[cluster];
    let instance = instances[entry >> 3u];
    let triangle = (entry & 7u) * kClusterTriangles + (vis - 1u) % kClusterTriangles;
    let o = occurrences[instance.x];
    let d = definitions[o.definition];
    let m = meshlets[d.meshletBase + instance.y];
    let last = (m.b.y & 0xFFu) - 1u;
    let packed = triangles[d.triangleBase + m.a.w + triangle];
    let i0 = m.a.z + min(packed & 0xFFu, last);
    let i1 = m.a.z + min((packed >> 8u) & 0xFFu, last);
    let i2 = m.a.z + min((packed >> 16u) & 0xFFu, last);
    let w0 = transformPoint(o, decodePosition(d, i0));
    let w1 = transformPoint(o, decodePosition(d, i1));
    let w2 = transformPoint(o, decodePosition(d, i2));
    let c0 = frame.viewProjection * vec4f(w0, 1.0);
    let c1 = frame.viewProjection * vec4f(w1, 1.0);
    let c2 = frame.viewProjection * vec4f(w2, 1.0);
    let ndc = vec2f(pixel.x / frame.viewport.x * 2.0 - 1.0, 1.0 - pixel.y / frame.viewport.y * 2.0);
    let p0 = c0.xy / c0.w;
    let e1 = c1.xy / c1.w - p0;
    let e2 = c2.xy / c2.w - p0;
    let dp = ndc - p0;
    let det = e1.x * e2.y - e1.y * e2.x;
    var s: Surface;
    s.occurrence = instance.x;
    s.id = o.idBase + m.b.x + localFace(d, i0) + 1u;
    if (abs(det) < 1e-20) {
        s.normal = normalize(cross(w1 - w0, w2 - w0));
        return s;
    }
    let b1 = (dp.x * e2.y - dp.y * e2.x) / det;
    let b2 = (e1.x * dp.y - e1.y * dp.x) / det;
    let q = vec3f((1.0 - b1 - b2) / c0.w, b1 / c1.w, b2 / c2.w);
    let b = q / (q.x + q.y + q.z);
    let n = b.x * decodeNormal(d, i0) + b.y * decodeNormal(d, i1) + b.z * decodeNormal(d, i2);
    s.normal = normalize(transformDirection(o, n));
    return s;
}

fn hash(x: u32) -> u32 {
    var h = x * 0x9E3779B1u;
    h ^= h >> 15u;
    h *= 0x85EBCA77u;
    h ^= h >> 13u;
    return h;
}

fn sheetColor(y: f32) -> vec3f {
    return mix(vec3f(0.96, 0.95, 0.92), vec3f(0.88, 0.87, 0.84), y / frame.viewport.y);
}

// Three soft bands, a part colour from its occurrence, a little rim. `n` is in view space.
fn shade(n: vec3f, occurrence: u32) -> vec3f {
    let h = hash(occurrence + 1u);
    let base = vec3f(f32(h & 0xFFu), f32((h >> 8u) & 0xFFu), f32((h >> 16u) & 0xFFu)) / 255.0 * 0.35 +
               vec3f(0.55, 0.6, 0.66);
    let light = normalize(vec3f(-0.35, 0.55, 0.75));
    let lit = dot(n, light);
    let a = smoothstep(0.45, 0.55, lit);
    let b = smoothstep(-0.05, 0.05, lit);
    var color = base * select(mix(0.62, 0.82, b), mix(0.82, 1.0, a), a > 0.0);
    return color + vec3f(0.08) * pow(1.0 - abs(n.z), 3.0);
}

const kInk = vec3f(0.12, 0.13, 0.15);

// Outlines where the body changes, searched outward over the line's width, heavier against the
// sheet; a fold where a body steps in depth in front of itself; occlusion blurred over neighbours
// on the same surface; the feature edges' ink; and the cast shadow on the sheet.
@fragment
fn compositeIndexed(in: FullOut) -> @location(0) vec4f {
    let last = vec2i(frame.viewport) - 1;
    let p = vec2i(in.position.xy);
    let c = textureLoad(surfaceTexture, p, 0).rg; // pick id, body
    let tag = textureLoad(tagTexture, p, 0).r;
    let ppp = frame.pixelsPerPoint;
    let silhouette = 2.0 * ppp;
    let partLine = 1.25 * ppp;
    var solid = 0.0;
    let reach = 0.5 * max(silhouette, partLine) + 1.0;
    for (var r = 1.0; r <= reach; r += 1.0) {
        // Eight directions, along the axes and the diagonals.
        let a = i32(r);
        let d = i32(round(0.70710678 * r));
        let axisDistance = r - 0.5;
        let diagonalDistance = 1.41421356 * f32(d) - 0.5;
        for (var k = 0; k < 8; k++) {
            let diagonal = (k & 1) == 1;
            let step = select(a, d, diagonal);
            // k: 0 +x, 1 +x+y, 2 +y, 3 -x+y, 4 -x, 5 -x-y, 6 -y, 7 +x-y
            let sx = select(select(0, 1, k == 0 || k == 1 || k == 7), -1, k >= 3 && k <= 5);
            let sy = select(select(0, 1, k >= 1 && k <= 3), -1, k >= 5);
            let n = textureLoad(tagTexture, clamp(p + vec2i(sx, sy) * step, vec2i(0), last), 0).r;
            if (n != tag) {
                let background = tag == 0u || n == 0u;
                solid = max(solid, lineCover(select(partLine, silhouette, background),
                                             select(axisDistance, diagonalDistance, diagonal)));
            }
        }
    }

    if (c.y == 0u) {
        // The sheet, with the shadow blurred over its half-resolution neighbours.
        var color = sheetColor(in.position.y);
        if ((frame.features & kFeatureShadow) != 0u) {
            let h = p / 2;
            let lastHalf = vec2i(textureDimensions(shadowTexture)) - 1;
            var shadow = 0.0;
            for (var y = -1; y <= 1; y++) {
                for (var x = -1; x <= 1; x++) {
                    let w = select(1.0, 2.0, x == 0) * select(1.0, 2.0, y == 0);
                    shadow += w * textureLoad(shadowTexture, clamp(h + vec2i(x, y) * 3, vec2i(0), lastHalf), 0).r;
                }
            }
            color *= 1.0 - 0.28 * shadow / 16.0;
        }
        return vec4f(mix(color, kInk, solid), 1.0);
    }

    // A body folding in front of itself: a step in depth, not a kink.
    let z = viewDepth(depthAt(p));
    if (solid < 1.0) {
        let s = max(1, i32(round(ppp)));
        let zl = viewDepth(depthAt(p - vec2i(s, 0)));
        let zr = viewDepth(depthAt(p + vec2i(s, 0)));
        let zd = viewDepth(depthAt(p + vec2i(0, s)));
        let zu = viewDepth(depthAt(p - vec2i(0, s)));
        let threshold = 10.0 * frame.worldPerPixel * f32(s);
        let stepX = abs(zl + zr - 2.0 * z);
        let stepY = abs(zd + zu - 2.0 * z);
        if ((stepX > threshold && stepX > 0.4 * abs(zr - zl)) ||
            (stepY > threshold && stepY > 0.4 * abs(zu - zd))) {
            solid = max(solid, 0.85);
        }
    }

    // Occlusion, blurred over the half-resolution neighbours on the same surface.
    var shadeAmount = 1.0;
    if ((frame.features & kFeatureOcclusion) != 0u) {
        let h = p / 2;
        let lastHalf = vec2i(textureDimensions(occlusionTexture)) - 1;
        let tolerance = 6.0 * frame.worldPerPixel + 1e-5;
        var sum = 0.0;
        var total = 0.0;
        for (var y = -1; y <= 1; y++) {
            for (var x = -1; x <= 1; x++) {
                let a = textureLoad(occlusionTexture, clamp(h + vec2i(x, y), vec2i(0), lastHalf), 0).rg;
                let same = a.y > 0.0 && abs(a.y - z) < tolerance;
                let w = select(1.0, 2.0, x == 0) * select(1.0, 2.0, y == 0) * select(0.02, 1.0, same);
                sum += w * a.x;
                total += w;
            }
        }
        shadeAmount = mix(1.0, sum / total, 0.8);
    }

    var ink = solid;
    if ((frame.features & kFeatureEdges) != 0u) {
        ink = max(ink, textureLoad(inkTexture, p, 0).r);
    }
    let n = decodeViewNormal(textureLoad(normalTexture, p, 0).rg);
    return vec4f(mix(shade(n, c.y - 1u) * shadeAmount, kInk, ink), 1.0);
}

@fragment
fn composite(in: FullOut) -> @location(0) vec4f {
    let size = vec2i(frame.viewport);
    let p = vec2i(in.position.xy);
    let vis = textureLoad(visTexture, p, 0).r;
    let sheet = sheetColor(in.position.y);
    if (vis == 0u) {
        return vec4f(sheet, 1.0);
    }
    let s = reconstruct(vis, in.position.xy);
    let n = (frame.viewRotation * vec4f(s.normal, 0.0)).xyz;

    // Three soft bands, a part colour from its occurrence, a little rim.
    let h = hash(s.occurrence + 1u);
    let base = vec3f(f32(h & 0xFFu), f32((h >> 8u) & 0xFFu), f32((h >> 16u) & 0xFFu)) / 255.0 * 0.35 +
               vec3f(0.55, 0.6, 0.66);
    let light = normalize(vec3f(-0.35, 0.55, 0.75));
    let lit = dot(n, light);
    let a = smoothstep(0.45, 0.55, lit);
    let b = smoothstep(-0.05, 0.05, lit);
    var color = base * select(mix(0.62, 0.82, b), mix(0.82, 1.0, a), a > 0.0);
    color += vec3f(0.08) * pow(1.0 - abs(n.z), 3.0);

    // Ink where the part changes or the model meets the sheet.
    let right = textureLoad(visTexture, min(p + vec2i(1, 0), size - 1), 0).r;
    let down = textureLoad(visTexture, min(p + vec2i(0, 1), size - 1), 0).r;
    var ink = right == 0u || down == 0u;
    if (!ink && right != vis) {
        ink = occurrenceOf(right) != s.occurrence;
    }
    if (!ink && down != vis) {
        ink = occurrenceOf(down) != s.occurrence;
    }
    if (ink) {
        color = vec3f(0.12, 0.13, 0.15);
    }
    return vec4f(color, 1.0);
}
)wgsl";

} // namespace geo::spike
