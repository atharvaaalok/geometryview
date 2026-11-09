// Shared by every WebGPU pass: the uniforms, laid out exactly as their C++ counterparts in
// shaders/shared/gpu_types.h, the scene's tables, and the decoding, culling and noise every pass
// uses. Ported from the Metal shaders' geometry.h, noise.h and gpu_types.h.

// ---- Records -------------------------------------------------------------------------------

// FrameUniforms. A vec3f is followed by a pad where C++ gives a float3 sixteen bytes.
struct Frame {
    viewProjection: mat4x4f,
    viewRotation: mat3x3f,
    eye: vec3f,
    right: vec3f,
    up: vec3f,
    forward: vec3f,
    pad0: f32,
    halfWidth: f32,
    halfHeight: f32,
    tanHalfX: f32,
    tanHalfY: f32,
    secHalfX: f32,
    secHalfY: f32,
    perspective: u32,
    hoverFirst: u32,
    hoverEnd: u32,
    ghostPickable: u32,
    hoverHides: u32,
    viewport: vec2f,
    edgeFloor: f32,
    worldPerPixel: f32,
    depthNear: f32,
    depthFar: f32,
    aoRadius: f32,
    aoSamples: u32,
    lightTravel: vec3f,
    pad1: f32,
    ground: f32,
    shadowVisibility: f32,
    sectionPlanes: array<vec4f, 6>,
    sectionPlaneCount: u32,
    seeThrough: f32,
    excludeShells: u32,
    sampleOffset: vec2f,
}

// StyleUniforms.
struct Style {
    sheetTop: vec3f,
    sheetBottom: vec3f,
    gridColor: vec3f,
    grainTint: vec3f,
    ink: vec3f,
    hover: vec3f,
    hideTint: vec3f,
    shadowColor: vec3f,
    lightExterior: vec3f,
    lightInterior: vec3f,
    sky: vec3f,
    floorColor: vec3f,
    keyLight: vec3f,
    band: vec3f,
    fillLight: vec3f,
    selection: vec4u,
    strips: vec2f,
    vignette: f32,
    gridAlpha: f32,
    gridPitch: f32,
    grain: f32,
    inkAlpha: f32,
    silhouette: f32,
    partLine: f32,
    crease: f32,
    highlight: f32,
    ghostInk: f32,
    ghostAlpha: f32,
    ghostLineAlpha: f32,
    hoverAmount: f32,
    ao: f32,
    shadowAlpha: f32,
    shadowSoft: f32,
    wobble: f32,
    wobblePeriod: f32,
    pixelsPerPoint: f32,
    sharpness: f32,
    exposure: f32,
    contrast: f32,
    ghostStyle: u32,
    shadowOn: u32,
    capInk: f32,
    metalDiffuse: f32,
    fold: f32,
    ghostLine: f32,
}

// One per definition. Geometry lives in one buffer of words, so a definition records where each
// of its streams starts, in words.
struct Definition {
    meshlets: u32,
    vertices: u32,
    highBits: u32,
    triangles: u32,
    edgeSegments: u32,
    edgeChunks: u32,
    vertexCount: u32, // for clamping what a cache file says, which is untrusted
    faceCount: u32,
    boundsMin: vec3f,
    boundsStep: f32, // the largest component of boundsScale
    decodeScale: vec3f,
    flags: u32,
    boundsScale: vec3f,
    meshletCount: u32,
    edgeLift: f32, // see GpuDefinition.edgeLift
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

// GpuOccurrence: a 3x4 transform by rows, its definition and its first pick id.
struct Occurrence {
    row0: vec4f,
    row1: vec4f,
    row2: vec4f,
    definition: u32,
    idBase: u32,
    pad0: u32,
    pad1: u32,
}

// An occurrence's OccurrenceState with its look: flags, where its face bytes start, the look.
struct State {
    flags: u32,
    facePage: u32,
    look: u32,
    pad0: u32,
}

// The cache's 32-byte Meshlet, decoded from its eight words.
struct Meshlet {
    center: vec3u, // on the definition's 16-bit bounds grid
    radius: u32,
    vertexOffset: u32,
    triangleOffset: u32,
    faceBase: u32,
    vertexCount: u32,
    triangleCount: u32,
    cone: vec3i,
    coneCutoff: i32,
}

// ---- Constants, as in gpu_records.h and gpu_types.h ------------------------------------------

const kDefinitionWide = 1u;
const kDefinitionOpen = 2u;
const kMaxSectionPlanes = 6u;

const kStateHidden = 1u;
const kStateGhost = 2u;
const kStateSelected = 4u;
const kStateFaceHidden = 16u;
const kStateFaceGhost = 32u;
const kStateFaceSelected = 64u;

const kTagCap = 0x80u;
const kTagShell = 0x80u;
const kTagValues = 127u;

const kLookShell = 0x8000u;
const kLookIndex = 0x7FFFu;

const kGhostShell = 1u;
const kGhostLines = 2u;

const kEdgeCrease = 0u;
const kEdgeSmooth = 1u;
const kEdgeBoundary = 2u;
const kEdgeChunkSegments = 32u;
const kEdgeNoFace = 0x3FFFFFFFu;
const kEdgeClassShift = 30u;

const kMeshletMaxVertices = 124u;
// An index of the surface passes is slot << kSlotShift | vertex within the meshlet.
const kSlotShift = 7u;

// ---- The scene, in group 0 -----------------------------------------------------------------

@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var<uniform> style: Style;
@group(0) @binding(2) var<storage, read> definitions: array<Definition>;
@group(0) @binding(3) var<storage, read> occurrences: array<Occurrence>;
@group(0) @binding(4) var<storage, read> geometry: array<u32>;
@group(0) @binding(5) var<storage, read> states: array<State>;
@group(0) @binding(6) var<storage, read> faceStates: array<u32>; // bytes, four to a word
@group(0) @binding(7) var<storage, read> palette: array<u32>;    // three RGBA8 texels per look

// ---- Decoding ------------------------------------------------------------------------------

fn transformPoint(o: Occurrence, p: vec3f) -> vec3f {
    let h = vec4f(p, 1.0);
    return vec3f(dot(o.row0, h), dot(o.row1, h), dot(o.row2, h));
}

fn transformDirection(o: Occurrence, v: vec3f) -> vec3f {
    return vec3f(dot(o.row0.xyz, v), dot(o.row1.xyz, v), dot(o.row2.xyz, v));
}

fn signedByte(word: u32, offset: u32) -> i32 {
    return extractBits(i32(word), offset, 8u);
}

fn meshletAt(d: Definition, index: u32) -> Meshlet {
    let w = d.meshlets + index * 8u;
    let w0 = geometry[w];
    let w1 = geometry[w + 1u];
    let w5 = geometry[w + 5u];
    let w6 = geometry[w + 6u];
    var m: Meshlet;
    m.center = vec3u(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu);
    m.radius = w1 >> 16u;
    m.vertexOffset = geometry[w + 2u];
    m.triangleOffset = geometry[w + 3u];
    m.faceBase = geometry[w + 4u];
    m.vertexCount = w5 & 0xFFu;
    m.triangleCount = (w5 >> 8u) & 0xFFu;
    m.cone = vec3i(signedByte(w5, 16u), signedByte(w5, 24u), signedByte(w6, 0u));
    m.coneCutoff = signedByte(w6, 8u);
    return m;
}

// A vertex's position in definition space. Integers first, so a vertex at the same grid point
// decodes to the same float wherever it is read, which keeps shared edges watertight and puts
// feature edges exactly on their surfaces. See ADR 0004.
fn decodePosition(d: Definition, vertexIndex: u32) -> vec3f {
    let w = d.vertices + vertexIndex * 3u;
    let w0 = geometry[w];
    let w1 = geometry[w + 1u];
    var grid = vec3u(w0 & 0xFFFFu, w0 >> 16u, w1 & 0xFFFFu);
    if ((d.flags & kDefinitionWide) != 0u) {
        let high = geometry[d.highBits + vertexIndex];
        grid |= vec3u(high & 0xFFu, (high >> 8u) & 0xFFu, (high >> 16u) & 0xFFu) << vec3u(16u);
    }
    return vec3f(grid) * d.decodeScale + d.boundsMin;
}

fn decodeOctahedral(word: u32) -> vec3f {
    let f = vec2f(f32(extractBits(i32(word), 0u, 16u)), f32(extractBits(i32(word), 16u, 16u))) /
            32767.0;
    var n = vec3f(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        n = vec3f((1.0 - abs(n.yx)) * sign(n.xy), n.z);
    }
    return normalize(n);
}

fn decodeNormal(d: Definition, vertexIndex: u32) -> vec3f {
    return decodeOctahedral(geometry[d.vertices + vertexIndex * 3u + 2u]);
}

fn localFace(d: Definition, vertexIndex: u32) -> u32 {
    return geometry[d.vertices + vertexIndex * 3u + 1u] >> 16u;
}

// A sphere's centre in world space, from bounds stored on the definition's 16-bit bounds grid.
fn boundsCenter(d: Definition, o: Occurrence, gridCenter: vec3u) -> vec3f {
    return transformPoint(o, vec3f(gridCenter) * d.boundsScale + d.boundsMin);
}

// Whether a sphere can touch the view.
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

fn faceByte(page: u32, face: u32) -> u32 {
    let at = page + face;
    return (faceStates[at >> 2u] >> ((at & 3u) * 8u)) & 0xFFu;
}

// The flags a face shows: its occurrence's own, combined with any of its own.
fn faceFlags(state: State, face: u32) -> u32 {
    return (state.flags & (kStateHidden | kStateGhost | kStateSelected)) |
           faceByte(state.facePage, face);
}

// Where a point's shadow lands on the ground.
fn shadowOf(p: vec3f) -> vec3f {
    let height = p.z - frame.ground;
    return p + frame.lightTravel * (height / max(-frame.lightTravel.z, 1e-3));
}

// ---- Noise, mirrored exactly on the CPU so the pointer can follow the wobble --------------------

fn noiseHash(v: vec2i) -> u32 {
    let x = u32(v.x + 32768);
    let y = u32(v.y + 32768);
    var h = (x * 0x8DA6B343u) ^ (y * 0xD8163841u);
    h ^= h >> 13u;
    h *= 0x5BD1E995u;
    h ^= h >> 15u;
    return h;
}

fn noiseAt(v: vec2i) -> f32 {
    return f32(noiseHash(v) & 0xFFFFu) / 65535.0;
}

// Smooth noise in [0, 1] with one feature per unit.
fn valueNoise(p: vec2f) -> f32 {
    let i = floor(p);
    let f = p - i;
    let u = f * f * (3.0 - 2.0 * f);
    let c = vec2i(i);
    return mix(mix(noiseAt(c), noiseAt(c + vec2i(1, 0)), u.x),
               mix(noiseAt(c + vec2i(0, 1)), noiseAt(c + vec2i(1, 1)), u.x), u.y);
}

// The wobble the composite applies to everything it samples, so lines follow it.
fn wobbleOffset(pixel: vec2f) -> vec2f {
    if (style.wobble <= 0.0) {
        return vec2f(0.0);
    }
    let q = pixel / style.wobblePeriod;
    return style.wobble * 2.0 *
           vec2f(valueNoise(q) - 0.5, valueNoise(q + vec2f(17.0, 9.0)) - 0.5);
}

// Distance along the view, from stored depth, which is reversed.
fn viewDepth(depth: f32) -> f32 {
    if (frame.perspective != 0u) {
        return frame.depthNear / max(depth, 1e-12);
    }
    return frame.depthFar - depth * (frame.depthFar - frame.depthNear);
}

fn pixelWorldAt(z: f32) -> f32 {
    return select(frame.worldPerPixel, frame.worldPerPixel * z, frame.perspective != 0u);
}

fn texelColor(texel: u32) -> vec3f {
    return unpack4x8unorm(texel).rgb;
}

// ---- The sheet, shared by the sheet pass and the shell's far walls ------------------------------

// A vertical wash, an optional vignette and drafting grid, and paper grain.
fn sheetColor(px: vec2f) -> vec3f {
    let uv = px / frame.viewport; // y runs down
    var color = mix(style.sheetTop, style.sheetBottom, uv.y);
    if (style.vignette > 0.0) {
        let r = length((uv - 0.5) * vec2f(frame.viewport.x / frame.viewport.y, 1.0));
        color *= 1.0 - style.vignette * smoothstep(0.3, 1.0, r);
    }
    // Derivatives before any branch, as WGSL requires.
    let cell = px / max(style.gridPitch, 1e-6);
    let minorWidth = fwidth(cell);
    let majorWidth = fwidth(cell / 5.0);
    if (style.gridAlpha > 0.0 && style.gridPitch > 0.0) {
        // Minor lines every pitch and heavier major lines every five, a pixel wide at any scale.
        let minor = abs(fract(cell - 0.5) - 0.5) / minorWidth;
        let major = abs(fract(cell / 5.0 - 0.5) - 0.5) / majorWidth;
        let line = max(1.0 - min(min(minor.x, minor.y), 1.0),
                       1.6 * (1.0 - min(min(major.x, major.y), 1.0)));
        color = mix(color, style.gridColor, style.gridAlpha * line);
    }
    // Paper tooth: a fine grain over a coarser one.
    let fine = valueNoise(px / (1.35 * style.pixelsPerPoint));
    let paper = 0.55 * fine + 0.45 * valueNoise(px / (4.0 * style.pixelsPerPoint));
    color = mix(color, style.grainTint, 0.11 * style.grain * paper);
    return color;
}
