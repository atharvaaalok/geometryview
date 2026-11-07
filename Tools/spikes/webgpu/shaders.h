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
    secHalfY: f32, instanceCount: u32, openClusterStart: u32, pad0: u32,
}

struct Definition {
    meshletBase: u32, vertexBase: u32, highBitsBase: u32, triangleBase: u32,
    boundsMin: vec3f, boundsStep: f32,
    decodeScale: vec3f, flags: u32,
    boundsScale: vec3f, vertexCount: u32,
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

@fragment
fn composite(in: FullOut) -> @location(0) vec4f {
    let size = vec2i(frame.viewport);
    let p = vec2i(in.position.xy);
    let vis = textureLoad(visTexture, p, 0).r;
    let sheet = mix(vec3f(0.96, 0.95, 0.92), vec3f(0.88, 0.87, 0.84), in.position.y / frame.viewport.y);
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
