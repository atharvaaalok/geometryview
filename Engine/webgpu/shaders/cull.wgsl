// Culling and compaction: one thread per meshlet instance, or per edge chunk instance, each frame.
// A visible meshlet gets a slot in each draw list it belongs to, which the vertex stage finds it
// by, and exactly as many indices as its triangles need, written by its workgroup together. An
// index is slot << kSlotShift | vertex within the meshlet, so the hardware reuses vertices.
//
// The surface lists keep the instances' order, so surfaces at equal depth, which a section
// exposes where parts touch, and overlapping ghosts draw the same way every frame: one pass counts
// what each workgroup adds to each list, one turns the counts into offsets, and one writes. The
// shadow and the edges, whose results do not depend on order, take one pass with atomics.
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
    groupCount: u32, // workgroups the surface passes dispatch
    pad0: u32,
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

// The draws' arguments, reset by the CPU before each frame's cull, then what each workgroup adds
// to each ordered list: slots, then indices, which the scan turns into where its writes start.
struct Counters {
    surfaces: array<DrawIndexed, 8>,
    edges: array<Draw, 2>,
    slots: array<atomic<u32>, 8>,
    edgeSlots: array<atomic<u32>, 2>,
    groups: array<u32>,
}

const kLists = 7u;
const kOrderedLists = 6u;
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
    // Wholly on the cut side of a plane, it would be clipped away.
    for (var k = 0u; k < frame.sectionPlaneCount; k++) {
        let plane = frame.sectionPlanes[k];
        if (dot(plane.xyz, center) + plane.w > radius) {
            return false;
        }
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

// The shadow's list, in whatever order workgroups finish: one pass, with atomics.
@compute @workgroup_size(64)
fn cullUnordered(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
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

// ---- The ordered surface lists ---------------------------------------------------------------

fn groupIndex(group: vec3u, groups: vec3u) -> u32 {
    return group.y * groups.x + group.x;
}

fn groupEntry(group: u32, list: u32) -> u32 {
    return (group * kOrderedLists + list) * 2u;
}

// What each workgroup adds to each list.
@compute @workgroup_size(64)
fn countSurfaces(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
                 @builtin(num_workgroups) groups: vec3u) {
    let index = instanceIndex(lid, group, groups);
    if (lid < kOrderedLists) {
        atomicStore(&slotCount[lid], 0u);
        atomicStore(&indexCount[lid], 0u);
    }
    workgroupBarrier();
    if (index < lists.instanceCount) {
        let instance = instances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        if (instance.y < d.meshletCount) {
            let m = meshletAt(d, instance.y);
            let bits = membership(instance.x, m, d, o);
            for (var list = 0u; list < kOrderedLists; list++) {
                if ((bits & (1u << list)) != 0u) {
                    atomicAdd(&slotCount[list], 1u);
                    atomicAdd(&indexCount[list], m.triangleCount * 3u);
                }
            }
        }
    }
    workgroupBarrier();
    if (lid < kOrderedLists) {
        let at = groupEntry(groupIndex(group, groups), lid);
        counters.groups[at] = atomicLoad(&slotCount[lid]);
        counters.groups[at + 1u] = atomicLoad(&indexCount[lid]);
    }
}

var<workgroup> scanScratch: array<u32, 256>;

// Turns the workgroups' counts into where each one's writes start, list by list, in one
// workgroup, and sets each list's draw to its total.
@compute @workgroup_size(256)
fn scanSurfaces(@builtin(local_invocation_index) lid: u32) {
    let groupCount = lists.groupCount;
    for (var list = 0u; list < kOrderedLists; list++) {
        for (var value = 0u; value < 2u; value++) {
            var carry = 0u;
            for (var base = 0u; base < groupCount; base += 256u) {
                let i = base + lid;
                var own = 0u;
                if (i < groupCount) {
                    own = counters.groups[groupEntry(i, list) + value];
                }
                scanScratch[lid] = own;
                workgroupBarrier();
                for (var step = 1u; step < 256u; step <<= 1u) {
                    var add = 0u;
                    if (lid >= step) {
                        add = scanScratch[lid - step];
                    }
                    workgroupBarrier();
                    scanScratch[lid] += add;
                    workgroupBarrier();
                }
                if (i < groupCount) {
                    counters.groups[groupEntry(i, list) + value] = carry + scanScratch[lid] - own;
                }
                carry += scanScratch[255];
                workgroupBarrier();
            }
            if (lid == 0u) {
                let region = lists.regions[list];
                if (value == 0u) {
                    atomicStore(&counters.slots[list], min(carry, region.slotCapacity));
                } else {
                    atomicStore(&counters.surfaces[list].indexCount,
                                min(carry, region.indexCapacity));
                }
            }
        }
    }
}

var<workgroup> threadSlots: array<u32, 384>;   // per thread and list: 1 if it joins
var<workgroup> threadIndices: array<u32, 384>; // and how many indices it brings

// Writes each workgroup's slots and indices where the scan said, its threads in order.
@compute @workgroup_size(64)
fn writeSurfaces(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
                 @builtin(num_workgroups) groups: vec3u) {
    let index = instanceIndex(lid, group, groups);
    var bits = 0u;
    var triangles = 0u;
    if (index < lists.instanceCount) {
        let instance = instances[index];
        let o = occurrences[instance.x];
        let d = definitions[o.definition];
        if (instance.y < d.meshletCount) {
            let m = meshletAt(d, instance.y);
            bits = membership(instance.x, m, d, o);
            triangles = m.triangleCount;
        }
    }
    for (var list = 0u; list < kOrderedLists; list++) {
        let joins = (bits & (1u << list)) != 0u;
        threadSlots[lid * kOrderedLists + list] = select(0u, 1u, joins);
        threadIndices[lid * kOrderedLists + list] = select(0u, triangles * 3u, joins);
    }
    workgroupBarrier();
    // One thread per list counts off the workgroup's threads in order.
    if (lid < kOrderedLists) {
        var slots = 0u;
        var indices = 0u;
        for (var t = 0u; t < 64u; t++) {
            let at = t * kOrderedLists + lid;
            let ownSlot = threadSlots[at];
            let ownIndices = threadIndices[at];
            threadSlots[at] = slots;
            threadIndices[at] = indices;
            slots += ownSlot;
            indices += ownIndices;
        }
    }
    workgroupBarrier();
    let entry = groupIndex(group, groups);
    var joined = 0u;
    for (var list = 0u; list < kOrderedLists; list++) {
        var slot = 0xFFFFFFFFu;
        var first = 0u;
        if ((bits & (1u << list)) != 0u && joined < 2u) {
            let region = lists.regions[list];
            let at = groupEntry(entry, list);
            let localSlot = counters.groups[at] + threadSlots[lid * kOrderedLists + list];
            let localIndex = counters.groups[at + 1u] + threadIndices[lid * kOrderedLists + list];
            // Past its capacity a list drops the rest rather than write beyond it.
            if (localSlot < region.slotCapacity &&
                localIndex + triangles * 3u <= region.indexCapacity) {
                slot = region.slotBase + localSlot;
                first = region.indexBase + localIndex;
                slotsOut[slot] = instances[index];
            }
        }
        if ((bits & (1u << list)) != 0u && joined < 2u) {
            wantSlot[lid * 2u + joined] = slot;
            wantIndex[lid * 2u + joined] = first;
            wantInstance[lid * 2u + joined] = index;
            joined++;
        }
    }
    for (var k = joined; k < 2u; k++) {
        wantSlot[lid * 2u + k] = 0xFFFFFFFFu;
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
