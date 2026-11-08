// Feature edges as screen-space quads: six vertices per segment of each visible edge chunk. Each
// segment reads the state of the faces on its sides to decide what it is: a crease on something
// solid, a highlight around a hovered or selected face, or a dashed line of a ghost. Coverage goes
// to the ink target, keeping the larger value, so overlapping quads at a joint never ink twice.
// Ported from edges.metal, whose mesh stage does per segment what this vertex stage does per
// corner.

@group(1) @binding(0) var<storage, read> edgeSlots: array<vec2u>; // occurrence, chunk

// The ghost-lines pass draws ghosts' dashed edges, after ghosts' skins. The other pass draws
// creases and highlights, before them.
override kGhostLinePass: bool = false;
override kSection: bool = false;

const kInked = 1u;       // a crease, or a ghost's line
const kHighlighted = 2u; // around a hovered or selected face
const kCorners = array<u32, 6>(0u, 1u, 2u, 2u, 1u, 3u);

struct EdgeVertex {
    @builtin(position) position: vec4f,
    @location(0) @interpolate(linear) across: f32, // pixels from the line's middle, signed
    @location(1) @interpolate(linear) along: f32,  // pixels along the edge's chain, for dashes
    @location(2) @interpolate(flat) kind: u32,
    @location(3) world: vec3f,
}

fn pixelWorldOf(p: vec3f) -> f32 {
    if (frame.perspective != 0u) {
        return frame.worldPerPixel * max(dot(p - frame.eye, frame.forward), 0.0);
    }
    return frame.worldPerPixel;
}

// Toward the eye by the coarsest chord error, or two points if that is more: an edge lies on its
// own faces exactly, but a neighbour's differently tessellated face can sit a hair in front of
// it, and the line would show in dashes.
fn lift(p: vec3f) -> vec3f {
    let offset = max(frame.lineOffset, 2.0 * style.pixelsPerPoint * pixelWorldOf(p));
    var toward = -frame.forward;
    if (frame.perspective != 0u) {
        toward = normalize(frame.eye - p);
    }
    return p + toward * offset;
}

@vertex
fn edgeVertex(@builtin(vertex_index) vertexId: u32) -> EdgeVertex {
    let slot = edgeSlots[vertexId / 192u];
    let index = (vertexId % 192u) / 6u;
    let k = kCorners[vertexId % 6u];

    var v: EdgeVertex;
    v.position = vec4f(0.0, 0.0, 2.0, 1.0);
    v.across = 0.0;
    v.along = 0.0;
    v.kind = 0u;
    v.world = vec3f(0.0);

    let o = occurrences[slot.x];
    let d = definitions[o.definition];
    let cw = d.edgeChunks + slot.y * 4u;
    let firstSegment = geometry[cw + 2u];
    let segments = min(geometry[cw + 3u], kEdgeChunkSegments);
    if (index >= segments) {
        return v;
    }
    // A cache file is untrusted, so what it says is clamped rather than read out of bounds.
    let sw = d.edgeSegments + (firstSegment + index) * 5u;
    let segmentA = geometry[sw];
    let segmentB = geometry[sw + 1u];
    let rawFaceA = geometry[sw + 2u];
    let rawFaceB = geometry[sw + 3u];
    let along = bitcast<f32>(geometry[sw + 4u]);
    let lastFace = max(d.faceCount, 1u) - 1u;
    let lastVertex = max(d.vertexCount, 1u) - 1u;
    let edgeClass = rawFaceB >> kEdgeClassShift;
    let faceA = min(rawFaceA, lastFace);
    let rawB = rawFaceB & kEdgeNoFace;
    let faceB = select(min(rawB, lastFace), faceA, rawB == kEdgeNoFace);

    let state = states[slot.x];
    let flagsA = faceFlags(state, faceA);
    let flagsB = faceFlags(state, faceB);
    let shownA = (flagsA & kStateHidden) == 0u;
    let shownB = (flagsB & kStateHidden) == 0u;
    let solid = (shownA && (flagsA & kStateGhost) == 0u) || (shownB && (flagsB & kStateGhost) == 0u);
    let idA = o.idBase + faceA + 1u;
    let idB = o.idBase + faceB + 1u;
    let hovered = (shownA && idA >= frame.hoverFirst && idA < frame.hoverEnd) ||
                  (shownB && idB >= frame.hoverFirst && idB < frame.hoverEnd);
    let selected = (shownA && (flagsA & kStateSelected) != 0u) ||
                   (shownB && (flagsB & kStateSelected) != 0u);

    var kind = 0u;
    if (kGhostLinePass) {
        kind = select(0u, kInked, !solid && (shownA || shownB) && edgeClass != kEdgeSmooth);
    } else {
        kind = select(0u, kInked, solid && edgeClass != kEdgeSmooth) |
               select(0u, kHighlighted, hovered || selected);
    }

    let a = transformPoint(o, decodePosition(d, min(segmentA, lastVertex)));
    let b = transformPoint(o, decodePosition(d, min(segmentB, lastVertex)));
    let clipA = frame.viewProjection * vec4f(lift(a), 1.0);
    let clipB = frame.viewProjection * vec4f(lift(b), 1.0);
    if (kind == 0u || clipA.w <= 1e-6 || clipB.w <= 1e-6) {
        return v;
    }

    let halfViewport = 0.5 * frame.viewport;
    let screenA = clipA.xy / clipA.w * halfViewport;
    let screenB = clipB.xy / clipB.w * halfViewport;
    let span = screenB - screenA;
    let spanLength = length(span);
    let direction = select(vec2f(1.0, 0.0), span / spanLength, spanLength > 1e-4);
    let normal = vec2f(-direction.y, direction.x);
    let width = max(select(0.0, style.crease, (kind & kInked) != 0u),
                    select(0.0, style.highlight, (kind & kHighlighted) != 0u));
    let halfWidth = 0.5 * width + 1.0; // a pixel more, for the soft edge

    // Arc length in pixels, so dashes keep their size and run on across segments.
    let scale = length(o.row0.xyz);
    let alongA = along * scale;
    let alongB = alongA + distance(a, b);

    let t = f32(k >> 1u);
    let side = select(-1.0, 1.0, (k & 1u) != 0u);
    var s = mix(screenA, screenB, t) + direction * (t * 2.0 - 1.0) * halfWidth * 0.5 +
            normal * side * halfWidth;
    // The composite samples at pixel + wobble, so a boundary appears shifted by minus the wobble
    // there; lines move with it.
    let pixel = vec2f(s.x + halfViewport.x, halfViewport.y - s.y);
    let wobble = wobbleOffset(pixel);
    s -= vec2f(wobble.x, -wobble.y);
    let clip = mix(clipA, clipB, t);
    v.position = vec4f(s / halfViewport * clip.w, clip.z, clip.w);
    v.across = side * halfWidth;
    v.along = mix(alongA / pixelWorldOf(a), alongB / pixelWorldOf(b), t);
    v.kind = kind;
    v.world = mix(a, b, t);
    return v;
}

@fragment
fn edgeFragment(in: EdgeVertex) -> @location(3) vec4f {
    // Metal's four samples share one evaluation at the pixel's centre, so a jittered sample moves
    // its position across and along the line back there.
    let toCentre = -frame.sampleOffset;
    let across = in.across + dpdx(in.across) * toCentre.x + dpdy(in.across) * toCentre.y;
    let alongHere = in.along + dpdx(in.along) * toCentre.x + dpdy(in.along) * toCentre.y;
    // Ghost lines are dashed, 4 on and 3 off in points: the drafting sign for what is behind.
    let period = 7.0 * style.pixelsPerPoint;
    let phase = fract(alongHere / period);
    let soft = fwidth(alongHere / period);
    if (kSection) {
        for (var p = 0u; p < frame.sectionPlaneCount; p++) {
            let plane = frame.sectionPlanes[p];
            if (dot(plane.xyz, in.world) + plane.w > 0.0) {
                discard;
            }
        }
    }
    let distance = abs(across);
    let crease = 1.0 - smoothstep(0.5 * style.crease - 0.5, 0.5 * style.crease + 0.5, distance);
    let highlight = 1.0 - smoothstep(0.5 * style.highlight - 0.5, 0.5 * style.highlight + 0.5,
                                     distance);
    var ink = vec4f(0.0);
    if (kGhostLinePass) {
        if ((in.kind & kInked) != 0u) {
            ink.g = crease * (1.0 - smoothstep(4.0 / 7.0 - soft, 4.0 / 7.0 + soft, phase));
        }
    } else {
        ink.r = select(0.0, crease, (in.kind & kInked) != 0u);
        ink.b = select(0.0, highlight, (in.kind & kHighlighted) != 0u);
    }
    return ink;
}
