// Culling and compaction: one thread per meshlet instance, or per edge chunk instance, each frame.
// A visible meshlet gets a slot in each draw list it belongs to, which the vertex stage finds it
// by, and exactly as many indices as its triangles need, written by its workgroup together.
//
// Indices are 16 bits: (slot within its chunk) << 7 | vertex within the meshlet. A list is drawn in
// chunks of 512 slots, each with its first slot << 7 as the base vertex, so the vertex stage sees
// slot << 7 | vertex, as if indices were 32 bits, at half the memory. Each meshlet has an even
// number of indices, padded with one empty triangle where it has an odd number of triangles, so
// no 32-bit word of indices is shared by two meshlets and each is written whole.
//
// The lists keep the instances' order, which also keeps a chunk's indices together, and surfaces
// at equal depth, which a section exposes where parts touch, draw the same way every frame. One
// pass counts what each workgroup adds to each list, one turns the counts into offsets, one
// writes, and one sets each chunk's draw.
//
// Lists, as the Metal renderer's passes keep or drop occurrences:
//   0, 1  solid surfaces, closed and open
//   2, 3  ghosts, closed and open: occurrences ghosted whole or in part
//   4, 5  the see-through shell, closed and open, while it fades
//   6     the cast shadow, culled against the shadow's projection
// The CPU sizes each list's region from the states it uploaded, so a list never overflows.

struct Region {
    slotBase: u32,
    slotCapacity: u32,
    indexBase: u32, // in 16-bit indices, and even
    indexCapacity: u32,
}

struct Lists {
    regions: array<Region, 8>,
    edgeRegions: array<Region, 2>,
    chunks: array<vec4u, 8>, // per list: its first chunk draw, and how many it has
    instanceCount: u32,
    edgeInstanceCount: u32,
    groupCount: u32, // workgroups the surface passes dispatch
    shadowOn: u32,
    groupsBase: u32, // where the workgroups' counts start in Counters.words
    chunkDraws: u32, // chunk draws over all lists
    pad0: u32,
    pad1: u32,
}

struct Draw {
    vertexCount: atomic<u32>,
    instanceCount: u32,
    firstVertex: u32,
    firstInstance: u32,
}

// Reset by the CPU before each frame's cull. `words` holds every chunk's draw, five words each,
// then what each workgroup adds to each list: slots, then indices.
struct Counters {
    edges: array<Draw, 2>,
    edgeSlots: array<atomic<u32>, 2>,
    slots: array<u32, 8>,   // each list's total, from the scan
    indices: array<u32, 8>, // in 16-bit indices
    words: array<u32>,
}

const kLists = 7u;
const kListShadow = 6u;
const kChunkSlots = 512u;
const kEdgeVertices = 192u; // six per segment, for a chunk of 32
// Edges of a chunk smaller than this on screen, in pixels of radius, would only blot it with ink,
// so they are left out; the outline from the tags still draws the part's shape.
const kMinChunkPixels = 3.0;

@group(1) @binding(0) var<uniform> lists: Lists;
@group(1) @binding(1) var<storage, read> instances: array<vec2u>; // occurrence, meshlet or chunk
@group(1) @binding(2) var<storage, read_write> slotsOut: array<vec2u>;
@group(1) @binding(3) var<storage, read_write> indicesOut: array<u32>; // two 16-bit indices each
@group(1) @binding(4) var<storage, read_write> counters: Counters;

// Whether a meshlet can show. `cone` is false where back faces are drawn: the shell's far walls,
// and closed parts while a section opens them.
fn meshletVisible(m: Meshlet, d: Definition, o: Occurrence, cone: bool) -> bool {
    let center = boundsCenter(d, o, m.center);
    let radius = f32(m.radius) * d.boundsStep;
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

// Whether a meshlet's shadow can fall in view. Flattened along a light 75 degrees up, a sphere
// stretches by under 4%.
fn shadowVisible(m: Meshlet, d: Definition, o: Occurrence) -> bool {
    let center = boundsCenter(d, o, m.center);
    let radius = f32(m.radius) * d.boundsStep;
    return sphereInView(shadowOf(center), radius * 1.05);
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
    // Ghosts cast no shadow.
    if (lists.shadowOn != 0u && !ghostWhole && shadowVisible(m, d, o)) {
        bits |= 1u << kListShadow;
    }
    return bits;
}

// A meshlet's indices, padded to an even count with an empty triangle.
fn paddedIndices(triangles: u32) -> u32 {
    return (triangles + (triangles & 1u)) * 3u;
}

fn instanceIndex(lid: u32, group: vec3u, groups: vec3u) -> u32 {
    return (group.y * groups.x + group.x) * 64u + lid;
}

fn groupIndex(group: vec3u, groups: vec3u) -> u32 {
    return group.y * groups.x + group.x;
}

fn groupEntry(group: u32, list: u32) -> u32 {
    return lists.groupsBase + (group * kLists + list) * 2u;
}

var<workgroup> slotCount: array<atomic<u32>, kLists>;
var<workgroup> indexCount: array<atomic<u32>, kLists>;

// What each workgroup adds to each list.
@compute @workgroup_size(64)
fn countSurfaces(@builtin(local_invocation_index) lid: u32, @builtin(workgroup_id) group: vec3u,
                 @builtin(num_workgroups) groups: vec3u) {
    let index = instanceIndex(lid, group, groups);
    if (lid < kLists) {
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
            for (var list = 0u; list < kLists; list++) {
                if ((bits & (1u << list)) != 0u) {
                    atomicAdd(&slotCount[list], 1u);
                    atomicAdd(&indexCount[list], paddedIndices(m.triangleCount));
                }
            }
        }
    }
    workgroupBarrier();
    if (lid < kLists) {
        let at = groupEntry(groupIndex(group, groups), lid);
        counters.words[at] = atomicLoad(&slotCount[lid]);
        counters.words[at + 1u] = atomicLoad(&indexCount[lid]);
    }
}

var<workgroup> scanScratch: array<u32, 256>;

// Turns the workgroups' counts into where each one's writes start, list by list, in one
// workgroup, and keeps each list's totals.
@compute @workgroup_size(256)
fn scanSurfaces(@builtin(local_invocation_index) lid: u32) {
    let groupCount = lists.groupCount;
    for (var list = 0u; list < kLists; list++) {
        for (var value = 0u; value < 2u; value++) {
            var carry = 0u;
            for (var base = 0u; base < groupCount; base += 256u) {
                let i = base + lid;
                var own = 0u;
                if (i < groupCount) {
                    own = counters.words[groupEntry(i, list) + value];
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
                    counters.words[groupEntry(i, list) + value] = carry + scanScratch[lid] - own;
                }
                carry += scanScratch[255];
                workgroupBarrier();
            }
            if (lid == 0u) {
                let region = lists.regions[list];
                if (value == 0u) {
                    counters.slots[list] = min(carry, region.slotCapacity);
                } else {
                    counters.indices[list] = min(carry, region.indexCapacity);
                }
            }
        }
    }
}

var<workgroup> threadSlots: array<u32, 448>;   // per thread and list: 1 if it joins
var<workgroup> threadIndices: array<u32, 448>; // and how many indices it brings
// Each thread's meshlet, for writing indices together: up to three lists per thread.
var<workgroup> wantSlot: array<u32, 192>;  // the list's local slot, or none
var<workgroup> wantIndex: array<u32, 192>; // where its indices start, in 16-bit indices
var<workgroup> wantInstance: array<u32, 192>;

// Writes each workgroup's slots and indices where the scan said, its threads in order, and where
// each chunk's indices start.
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
    let own = paddedIndices(triangles);
    for (var list = 0u; list < kLists; list++) {
        let joins = (bits & (1u << list)) != 0u;
        threadSlots[lid * kLists + list] = select(0u, 1u, joins);
        threadIndices[lid * kLists + list] = select(0u, own, joins);
    }
    workgroupBarrier();
    // One thread per list counts off the workgroup's threads in order.
    if (lid < kLists) {
        var slots = 0u;
        var indices = 0u;
        for (var t = 0u; t < 64u; t++) {
            let at = t * kLists + lid;
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
    for (var list = 0u; list < kLists; list++) {
        if ((bits & (1u << list)) == 0u || joined >= 3u) {
            continue;
        }
        let region = lists.regions[list];
        let at = groupEntry(entry, list);
        let localSlot = counters.words[at] + threadSlots[lid * kLists + list];
        let localIndex = counters.words[at + 1u] + threadIndices[lid * kLists + list];
        var slot = 0xFFFFFFFFu;
        // Past its capacity a list drops the rest rather than write beyond it.
        if (localSlot < region.slotCapacity && localIndex + own <= region.indexCapacity) {
            slot = localSlot;
            slotsOut[region.slotBase + localSlot] = instances[index];
            // The first slot of a chunk says where the chunk's indices start.
            if (localSlot % kChunkSlots == 0u) {
                let draw = lists.chunks[list].x + localSlot / kChunkSlots;
                counters.words[draw * 5u + 2u] = region.indexBase + localIndex;
            }
        }
        wantSlot[lid * 3u + joined] = slot;
        wantIndex[lid * 3u + joined] = region.indexBase + localIndex;
        wantInstance[lid * 3u + joined] = index;
        joined++;
    }
    for (var k = joined; k < 3u; k++) {
        wantSlot[lid * 3u + k] = 0xFFFFFFFFu;
    }
    workgroupBarrier();

    // Together, write each visible meshlet's indices, two to a word.
    for (var v = 0u; v < 192u; v++) {
        let localSlot = wantSlot[v];
        if (localSlot == 0xFFFFFFFFu) {
            continue;
        }
        let instance = instances[wantInstance[v]];
        let d = definitions[occurrences[instance.x].definition];
        let m = meshletAt(d, instance.y);
        let count = m.triangleCount * 3u;
        let words = paddedIndices(m.triangleCount) / 2u;
        // Clamped because a cache file is untrusted and per-index validation would read every
        // page.
        let last = max(m.vertexCount, 1u) - 1u;
        let slotBits = (localSlot % kChunkSlots) << kSlotShift;
        let firstWord = wantIndex[v] / 2u;
        for (var w = lid; w < words; w += 64u) {
            var pair = 0u;
            for (var half = 0u; half < 2u; half++) {
                let j = w * 2u + half;
                var vertex = 0u; // the padding triangle sits on the meshlet's first vertex
                if (j < count) {
                    let packed = geometry[d.triangles + m.triangleOffset + j / 3u];
                    vertex = min(extractBits(packed, (j % 3u) * 8u, 8u), last);
                }
                pair |= (slotBits | vertex) << (half * 16u);
            }
            indicesOut[firstWord + w] = pair;
        }
    }
}

// Each chunk's draw: from where its indices start to where the next chunk's do, or the list's end.
@compute @workgroup_size(64)
fn chunkDraws(@builtin(global_invocation_id) id: vec3u) {
    let draw = id.x;
    if (draw >= lists.chunkDraws) {
        return;
    }
    var list = 0u;
    for (var l = 0u; l < kLists; l++) {
        let chunks = lists.chunks[l];
        if (draw >= chunks.x && draw < chunks.x + chunks.y) {
            list = l;
        }
    }
    let chunk = draw - lists.chunks[list].x;
    let total = counters.slots[list];
    var count = 0u;
    if (chunk * kChunkSlots < total) {
        let first = counters.words[draw * 5u + 2u];
        var end = lists.regions[list].indexBase + counters.indices[list];
        if ((chunk + 1u) * kChunkSlots < total) {
            end = counters.words[(draw + 1u) * 5u + 2u];
        }
        count = end - first;
    }
    counters.words[draw * 5u] = count;
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
