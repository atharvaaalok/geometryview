// Culling and compaction: one thread per meshlet instance, or per edge chunk instance, each frame.
// A visible meshlet gets a slot in each draw list it belongs to, which the vertex stage finds it
// by, and exactly as many indices as its triangles need, written by its workgroup together. An
// index is slot << kSlotShift | vertex within the meshlet, so the hardware reuses vertices.
//
// Lists, as the Metal renderer's passes keep or drop occurrences:
//   0, 1  solid surfaces, closed and open
//   2, 3  ghosts, closed and open: occurrences ghosted whole or in part
//   4, 5  the see-through shell, closed and open, while it fades
//   6     the cast shadow (the shadow variant)
// The CPU sizes each list's region from the states it uploaded, so a list never overflows.

struct Region {
    slotBase: u32,
    slotCapacity: u32,
    indexBase: u32,
    indexCapacity: u32,
}

struct Lists {
    regions: array<Region, 8>,
    edgeRegions: array<Region, 2>,
    instanceCount: u32,
    edgeInstanceCount: u32,
    pad0: u32,
    pad1: u32,
}

struct DrawIndexed {
    indexCount: atomic<u32>,
    instanceCount: u32,
    firstIndex: u32,
    baseVertex: i32,
    firstInstance: u32,
}

struct Draw {
    vertexCount: atomic<u32>,
    instanceCount: u32,
    firstVertex: u32,
    firstInstance: u32,
}

// Reset by the CPU before each frame's cull.
struct Counters {
    surfaces: array<DrawIndexed, 8>,
    edges: array<Draw, 2>,
    slots: array<atomic<u32>, 8>,
    edgeSlots: array<atomic<u32>, 2>,
}

const kLists = 7u;
const kListShadow = 6u;
const kEdgeVertices = 192u; // six per segment, for a chunk of 32
// Edges of a chunk smaller than this on screen, in pixels of radius, would only blot it with ink,
// so they are left out; the outline from the tags still draws the part's shape.
const kMinChunkPixels = 3.0;

@group(1) @binding(0) var<uniform> lists: Lists;
@group(1) @binding(1) var<storage, read> instances: array<vec2u>; // occurrence, meshlet or chunk
@group(1) @binding(2) var<storage, read_write> slotsOut: array<vec2u>;
@group(1) @binding(3) var<storage, read_write> indicesOut: array<u32>;
@group(1) @binding(4) var<storage, read_write> counters: Counters;

// The shadow variant culls against the shadow's projection and fills only its own list.
override kShadowCull: bool = false;

// Whether a meshlet can show. `cone` is false where back faces are drawn: the shell's far walls,
// and closed parts while a section opens them.
fn meshletVisible(m: Meshlet, d: Definition, o: Occurrence, cone: bool) -> bool {
    let center = boundsCenter(d, o, m.center);
    let radius = f32(m.radius) * d.boundsStep;
    if (kShadowCull) {
        // Flattened along a light 75 degrees up, a sphere stretches by under 4%.
        return sphereInView(shadowOf(center), radius * 1.05);
    }
    if (!sphereInView(center, radius)) {
        return false;
    }
    // An open surface shows both sides, so a normal cone proves nothing about it.
    if (!cone || (d.flags & kDefinitionOpen) != 0u) {
        return true;
    }
    let axis = transformDirection(o, vec3f(m.cone) / 127.0);
    let cutoff = f32(m.coneCutoff) / 127.0;
    let offset = center - frame.eye;
    if (frame.perspective != 0u) {
        return dot(offset, axis) < cutoff * length(offset) + radius;
    }
    return dot(frame.forward, axis) < cutoff;
}

// The lists an instance joins this frame, one bit each.
fn membership(occurrence: u32, m: Meshlet, d: Definition, o: Occurrence) -> u32 {
    let state = states[occurrence];
    let flags = state.flags;
    if ((flags & kStateHidden) != 0u) {
        return 0u;
    }
    let open = select(0u, 1u, (d.flags & kDefinitionOpen) != 0u);
    let ghostWhole = (flags & kStateGhost) != 0u;
    if (kShadowCull) {
        // Ghosts cast no shadow.
        return select(0u, 1u << kListShadow, !ghostWhole && meshletVisible(m, d, o, true));
    }
    let shell = (state.look & kLookShell) != 0u && frame.seeThrough > 0.0;
    let section = frame.sectionPlaneCount > 0u;
    var bits = 0u;
    if (!ghostWhole && !shell && meshletVisible(m, d, o, !section)) {
        bits |= 1u << open;
    }
    if ((flags & (kStateGhost | kStateFaceGhost)) != 0u && meshletVisible(m, d, o, !section)) {
        bits |= 1u << (2u + open);
    }
    if (!ghostWhole && shell && meshletVisible(m, d, o, false)) {
        bits |= 1u << (4u + open);
    }
    return bits;
}

var<workgroup> slotCount: array<atomic<u32>, kLists>;
var<workgroup> indexCount: array<atomic<u32>, kLists>;
var<workgroup> slotBase: array<u32, kLists>;
var<workgroup> indexBase: array<u32, kLists>;
// Each thread's meshlet, for writing indices together: up to two lists per thread.
var<workgroup> wantSlot: array<u32, 128>;
var<workgroup> wantIndex: array<u32, 128>;
var<workgroup> wantInstance: array<u32, 128>;

fn instanceIndex(lid: u32, group: vec3u, groups: vec3u) -> u32 {
    return (group.y * groups.x + group.x) * 64u + lid;
}

@compute @workgroup_size(64)
fn cullSurfaces(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
                @builtin(num_workgroups) groups: vec3u) {
    let index = instanceIndex(lid, group, groups);
    if (lid < kLists) {
        atomicStore(&slotCount[lid], 0u);
        atomicStore(&indexCount[lid], 0u);
    }
    workgroupBarrier();

    var bits = 0u;
    var localSlot = array<u32, 2>(0u, 0u);
    var localIndex = array<u32, 2>(0u, 0u);
    var listOf = array<u32, 2>(0u, 0u);
    var joined = 0u;
    if (index < lists.instanceCount) {
        let instance = instances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        if (instance.y < d.meshletCount) {
            let m = meshletAt(d, instance.y);
            bits = membership(instance.x, m, d, o);
            for (var list = 0u; list < kLists; list++) {
                if ((bits & (1u << list)) != 0u && joined < 2u) {
                    listOf[joined] = list;
                    localSlot[joined] = atomicAdd(&slotCount[list], 1u);
                    localIndex[joined] = atomicAdd(&indexCount[list], m.triangleCount * 3u);
                    joined++;
                }
            }
        }
    }
    workgroupBarrier();
    // One global reservation per list for the whole workgroup.
    if (lid < kLists) {
        let slots = atomicLoad(&slotCount[lid]);
        let indices = atomicLoad(&indexCount[lid]);
        var slot = 0u;
        var first = 0u;
        if (slots > 0u) {
            slot = atomicAdd(&counters.slots[lid], slots);
            first = atomicAdd(&counters.surfaces[lid].indexCount, indices);
        }
        let region = lists.regions[lid];
        // Past its capacity a list drops the rest rather than write beyond it.
        let fits = slot + slots <= region.slotCapacity && first + indices <= region.indexCapacity;
        slotBase[lid] = select(0xFFFFFFFFu, region.slotBase + slot, fits);
        indexBase[lid] = region.indexBase + first;
    }
    workgroupBarrier();
    for (var k = 0u; k < 2u; k++) {
        var slot = 0xFFFFFFFFu;
        if (k < joined && slotBase[listOf[k]] != 0xFFFFFFFFu) {
            slot = slotBase[listOf[k]] + localSlot[k];
            slotsOut[slot] = instances[index];
        }
        wantSlot[lid * 2u + k] = slot;
        wantIndex[lid * 2u + k] = select(0u, indexBase[listOf[k]] + localIndex[k], k < joined);
        wantInstance[lid * 2u + k] = index;
    }
    workgroupBarrier();

    // Together, write each visible meshlet's indices.
    for (var v = 0u; v < 128u; v++) {
        let slot = wantSlot[v];
        if (slot == 0xFFFFFFFFu) {
            continue;
        }
        let instance = instances[wantInstance[v]];
        let d = definitions[occurrences[instance.x].definition];
        let m = meshletAt(d, instance.y);
        let count = m.triangleCount * 3u;
        // Clamped because a cache file is untrusted and per-index validation would read every
        // page.
        let last = max(m.vertexCount, 1u) - 1u;
        let first = wantIndex[v];
        for (var j = lid; j < count; j += 64u) {
            let packed = geometry[d.triangles + m.triangleOffset + j / 3u];
            let vertex = min(extractBits(packed, (j % 3u) * 8u, 8u), last);
            indicesOut[first + j] = (slot << kSlotShift) | vertex;
        }
    }
}

// Edge chunks: list 0 creases and highlights, list 1 ghosts' dashed lines.
var<workgroup> edgeCount: array<atomic<u32>, 2>;
var<workgroup> edgeBase: array<u32, 2>;

fn chunkInView(o: Occurrence, d: Definition, chunk: u32) -> bool {
    let w = d.edgeChunks + chunk * 4u;
    let w0 = geometry[w];
    let w1 = geometry[w + 1u];
    let center = boundsCenter(d, o, vec3u(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu));
    let radius = f32(w1 >> 16u) * d.boundsStep;
    var pixelWorld = frame.worldPerPixel;
    if (frame.perspective != 0u) {
        pixelWorld = frame.worldPerPixel * max(dot(center - frame.eye, frame.forward), radius);
    }
    return radius >= kMinChunkPixels * pixelWorld && sphereInView(center, radius);
}

@compute @workgroup_size(64)
fn cullEdges(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
             @builtin(num_workgroups) groups: vec3u) {
    let index = instanceIndex(lid, group, groups);
    if (lid < 2u) {
        atomicStore(&edgeCount[lid], 0u);
    }
    workgroupBarrier();
    var local = array<u32, 2>(0xFFFFFFFFu, 0xFFFFFFFFu);
    var instance = vec2u(0u);
    if (index < lists.edgeInstanceCount) {
        instance = instances[lists.instanceCount + index];
        let flags = states[instance.x].flags;
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        if ((flags & kStateHidden) == 0u && chunkInView(o, d, instance.y)) {
            local[0] = atomicAdd(&edgeCount[0], 1u);
            if ((style.ghostStyle & kGhostLines) != 0u &&
                (flags & (kStateGhost | kStateFaceGhost)) != 0u) {
                local[1] = atomicAdd(&edgeCount[1], 1u);
            }
        }
    }
    workgroupBarrier();
    if (lid < 2u) {
        let count = atomicLoad(&edgeCount[lid]);
        var slot = 0u;
        if (count > 0u) {
            slot = atomicAdd(&counters.edgeSlots[lid], count);
            atomicAdd(&counters.edges[lid].vertexCount, count * kEdgeVertices);
        }
        let region = lists.edgeRegions[lid];
        edgeBase[lid] = select(0xFFFFFFFFu, region.slotBase + slot,
                               slot + count <= region.slotCapacity);
    }
    workgroupBarrier();
    for (var k = 0u; k < 2u; k++) {
        if (local[k] != 0xFFFFFFFFu && edgeBase[k] != 0xFFFFFFFFu) {
            slotsOut[edgeBase[k] + local[k]] = instance; // edge regions follow the surfaces'
        }
    }
}
