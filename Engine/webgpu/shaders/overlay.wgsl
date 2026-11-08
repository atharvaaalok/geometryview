// The view cube and the scale bar, over the model at the end of each main pass, in lockstep with
// the camera, with no depth test. Ported from overlay.metal. They are placed in pixels, so each
// jittered sample moves them by its own offset, as it moves the model through its matrix.

struct OverlayUniforms {
    viewRotation: mat3x3f, // world to view, so the cube turns with the camera
    viewport: vec2f,       // pixels
    cubeCenter: vec2f,     // pixels
    cubeHalf: f32,         // half the cube's edge, in pixels
    lineWidth: f32,        // the ink edge, in pixels
    cubeHover: u32,        // face index plus one, or zero
    quadCount: u32,
}

// A screen-space rectangle in pixels, from the top left. Solid if uv.z < 0, else coverage from the
// atlas region uv.xy to uv.zw tints it.
struct OverlayQuad {
    origin: vec2f,
    size: vec2f,
    uv: vec4f,
    color: vec4f,
}

@group(1) @binding(0) var<uniform> overlay: OverlayUniforms;
@group(1) @binding(1) var<storage, read> quads: array<OverlayQuad>;
@group(1) @binding(2) var atlas: texture_2d<f32>;
@group(1) @binding(3) var atlasSampler: sampler;

const kAtlasSize = vec2f(512.0, 336.0);
const kAtlasLabelCell = vec2f(144.0, 48.0); // one row per cube face
const kCubeIdBase = 0xFFFFFF00u;

// Cube faces with Z up: the outward normal, then the label's right and up directions, in the order
// of nav::StandardView's faces: front, back, left, right, top, bottom.
const kFaceNormal = array<vec3f, 6>(vec3f(0.0, -1.0, 0.0), vec3f(0.0, 1.0, 0.0),
                                    vec3f(-1.0, 0.0, 0.0), vec3f(1.0, 0.0, 0.0),
                                    vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, -1.0));
const kFaceRight = array<vec3f, 6>(vec3f(1.0, 0.0, 0.0), vec3f(-1.0, 0.0, 0.0),
                                   vec3f(0.0, -1.0, 0.0), vec3f(0.0, 1.0, 0.0),
                                   vec3f(1.0, 0.0, 0.0), vec3f(1.0, 0.0, 0.0));
const kFaceUp = array<vec3f, 6>(vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, 1.0),
                                vec3f(0.0, 0.0, 1.0), vec3f(0.0, 1.0, 0.0),
                                vec3f(0.0, -1.0, 0.0));
const kQuadCorner = array<vec2f, 6>(vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),
                                    vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));

const kLight = vec3f(0.561, 0.809, 0.175);
const kLightTone = vec3f(243.0, 241.0, 236.0) / 255.0;
const kMidTone = vec3f(214.0, 210.0, 200.0) / 255.0;
const kDarkTone = vec3f(191.0, 186.0, 174.0) / 255.0;
const kOverlayInk = vec3f(35.0, 30.0, 22.0) / 255.0;
const kCream = vec3f(255.0, 244.0, 214.0) / 255.0;

// The overlay lies over the model, so it takes away the ink beneath it, and the cube hides the
// tags of whatever is behind it from the outlines.
struct OverlayOutput {
    @location(0) color: vec4f,
    @location(1) id: u32,
    @location(2) depth: f32,
    @location(3) ink: vec4f, // blended to keep only 1 - alpha of what is there
    @location(4) tags: vec2u,
}

// A pixel position to clip space, moved by this sample's offset.
fn toClip(pixel: vec2f) -> vec4f {
    let p = pixel - frame.sampleOffset;
    return vec4f(p.x / overlay.viewport.x * 2.0 - 1.0, 1.0 - p.y / overlay.viewport.y * 2.0, 0.0,
                 1.0);
}

struct CubeVertex {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f, // -1 to 1 across the face
    @location(1) @interpolate(flat) normal: vec3f,
    @location(2) @interpolate(flat) face: u32,
}

@vertex
fn cubeVertex(@builtin(vertex_index) vertexId: u32) -> CubeVertex {
    let face = vertexId / 6u;
    let corner = kQuadCorner[vertexId % 6u];
    let world = kFaceNormal[face] + kFaceRight[face] * corner.x + kFaceUp[face] * corner.y;
    let view = overlay.viewRotation * world;
    let normal = overlay.viewRotation * kFaceNormal[face];
    var out: CubeVertex;
    out.position = toClip(overlay.cubeCenter + vec2f(view.x, -view.y) * overlay.cubeHalf);
    // Faces turned away are dropped; the cube is convex, so no depth test is needed.
    if (normal.z <= 1e-4) {
        out.position = vec4f(0.0, 0.0, 2.0, 1.0);
    }
    out.local = corner;
    out.normal = normal;
    out.face = face;
    return out;
}

// A varying moved from this sample's position back to the pixel's centre, where Metal evaluates
// it once for all four samples.
fn atCentre(value: vec2f) -> vec2f {
    let toCentre = -frame.sampleOffset;
    return value + dpdx(value) * toCentre.x + dpdy(value) * toCentre.y;
}

@fragment
fn cubeFragment(in: CubeVertex) -> OverlayOutput {
    let local = atCentre(in.local);
    // An ink edge of constant width in pixels, from how fast the face coordinate changes.
    let width = max(fwidth(in.local), vec2f(1e-5));
    let d = dot(normalize(in.normal), kLight);
    var color = select(select(kDarkTone, kMidTone, d > -0.45), kLightTone, d > -0.15);
    if (overlay.cubeHover == in.face + 1u) {
        color = kCream;
    }
    // The label fills the middle of the face, from its row of the atlas.
    let inLabel = local / vec2f(0.9, 0.3);
    if (all(abs(inLabel) <= vec2f(1.0))) {
        let texel = vec2f((inLabel.x * 0.5 + 0.5) * kAtlasLabelCell.x,
                          (0.5 - inLabel.y * 0.5) * kAtlasLabelCell.y +
                              f32(in.face) * kAtlasLabelCell.y);
        let coverage = textureSampleLevel(atlas, atlasSampler, texel / kAtlasSize, 0.0).r;
        color = mix(color, kOverlayInk, coverage);
    }
    let edge = (1.0 - abs(local)) / width;
    let ink = 1.0 - smoothstep(overlay.lineWidth - 0.5, overlay.lineWidth + 0.5,
                               min(edge.x, edge.y));
    color = mix(color, kOverlayInk, ink);
    return OverlayOutput(vec4f(color, 1.0), kCubeIdBase + in.face, 0.0, vec4f(0.0, 0.0, 0.0, 1.0),
                         vec2u(0u));
}

struct QuadVertex {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) @interpolate(flat) color: vec4f,
    @location(2) @interpolate(flat) solid: u32,
}

@vertex
fn quadVertex(@builtin(vertex_index) vertexId: u32,
              @builtin(instance_index) instance: u32) -> QuadVertex {
    let quad = quads[instance];
    let corner = kQuadCorner[vertexId] * 0.5 + 0.5;
    var out: QuadVertex;
    out.position = toClip(quad.origin + corner * quad.size);
    out.solid = select(0u, 1u, quad.uv.z < 0.0);
    out.uv = mix(quad.uv.xy, quad.uv.zw, vec2f(corner.x, 1.0 - corner.y));
    out.color = quad.color;
    return out;
}

@fragment
fn quadFragment(in: QuadVertex) -> OverlayOutput {
    let uv = atCentre(in.uv);
    var coverage = 1.0;
    if (in.solid == 0u) {
        coverage = textureSampleLevel(atlas, atlasSampler, uv, 0.0).r;
    }
    let alpha = in.color.a * coverage;
    return OverlayOutput(vec4f(in.color.rgb, alpha), 0u, 0.0, vec4f(0.0, 0.0, 0.0, alpha),
                         vec2u(0u));
}
