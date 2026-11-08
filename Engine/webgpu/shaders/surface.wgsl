// The surface passes: the vertex stage pulls each vertex from the cache's own records, by the slot
// the cull gave its meshlet, and the fragment stage shades as the Metal renderer's surfaceFragment
// does. Override constants choose the pass, as Metal's function constants do.
//
// Section planes clip by discarding, where Metal uses clip distances, since Safari has none; only
// the section pipelines pay for it.

@group(1) @binding(0) var<storage, read> slots: array<vec2u>; // occurrence, meshlet
@group(1) @binding(1) var frontIds: texture_2d<u32>;          // the peel pass's front surface
@group(1) @binding(2) var frontDepths: texture_2d<f32>;

// The ghost pass draws only ghosted faces, blended over everything opaque.
override kGhostPass: bool = false;
// Studio light with metal and roughness, rather than three flat bands.
override kPhysical: bool = false;
// The cast shadow: every solid face flattened onto the ground along the light.
override kShadowPass: bool = false;
// Section planes are on: geometry is clipped, and back faces seen through a cut are caps.
override kSection: bool = false;
// Classifying the shell: ids name occurrences rather than faces, and the peel pass keeps only the
// nearest surface of another part behind the one in front.
override kOccurrenceIds: bool = false;
override kPeel: bool = false;
// Which see-through layer this is: 0 none, 1 the shell's far walls, 2 its nearest front faces into
// depth only, 3 those faces again, blended.
override kShellLayer: u32 = 0u;

// Set in a vertex's flags when its definition is not closed, so a cut through it has no cap, and
// when the occurrence is part of the see-through shell.
const kFlagOpen = 0x80u;
const kFlagShell = 0x08u;

struct SurfaceVertex {
    @builtin(position) position: vec4f,
    @location(0) normal: vec3f,
    @location(1) @interpolate(flat) id: u32,
    // State flags in the low byte, the body's tag in the next, its look above.
    @location(2) @interpolate(flat) flags: u32,
    @location(3) world: vec3f, // for clipping by section planes
}

@vertex
fn surfaceVertex(@builtin(vertex_index) index: u32) -> SurfaceVertex {
    let slot = slots[index >> kSlotShift];
    let o = occurrences[slot.x];
    let d = definitions[o.definition];
    let m = meshletAt(d, slot.y);
    let vertexIndex = m.vertexOffset + min(index & ((1u << kSlotShift) - 1u),
                                           max(m.vertexCount, 1u) - 1u);
    let world = transformPoint(o, decodePosition(d, vertexIndex));
    let normal = transformDirection(o, decodeNormal(d, vertexIndex));
    let state = states[slot.x];
    let face = m.faceBase + localFace(d, vertexIndex);
    let flags = faceFlags(state, face);

    var out: SurfaceVertex;
    out.position = frame.viewProjection * vec4f(world, 1.0);
    out.normal = frame.viewRotation * normal;
    out.id = o.idBase + face + 1u;
    out.world = world;
    // Neighbouring bodies have different tags, unless their indices differ by a multiple of 127.
    let tag = 1u + slot.x % kTagValues;
    let open = select(0u, kFlagOpen, (d.flags & kDefinitionOpen) != 0u);
    let shell = (state.look & kLookShell) != 0u;
    out.flags = flags | open | select(0u, kFlagShell, shell) | (tag << 8u) |
                ((state.look & kLookIndex) << 16u);
    if (kOccurrenceIds) {
        out.id = slot.x + 1u;
    }
    // Faces never share vertices, so moving all of a face's vertices to one point outside the view
    // removes exactly that face, with no discard.
    let ghost = (flags & kStateGhost) != 0u;
    // While see-through fades the shell, opaque passes leave it out and the shell layers draw it.
    let fading = frame.seeThrough > 0.0;
    var dropped = (flags & kStateHidden) != 0u || ghost != kGhostPass;
    if (kOccurrenceIds) {
        dropped = dropped || (shell && frame.excludeShells != 0u);
    } else if (kShellLayer == 0u) {
        dropped = dropped || (shell && fading && !kGhostPass && !kShadowPass);
    } else {
        dropped = dropped || !shell || !fading;
    }
    if (dropped) {
        out.position = vec4f(0.0, 0.0, 2.0, 1.0);
    } else if (kShadowPass) {
        // Held at mid depth, since the flattened model can reach outside the depth range.
        let clip = frame.viewProjection * vec4f(shadowOf(world), 1.0);
        out.position = vec4f(clip.xy, 0.5 * clip.w, clip.w);
    }
    return out;
}

fn sectionCuts(world: vec3f) -> bool {
    for (var k = 0u; k < frame.sectionPlaneCount; k++) {
        let plane = frame.sectionPlanes[k];
        if (dot(plane.xyz, world) + plane.w > 0.0) {
            return true;
        }
    }
    return false;
}

// ---- Flat bands, after Colorfigures --------------------------------------------------------

// Three flat tones with a soft step between them. The step widens with the normal's rate of
// change, so curved parts stay smooth and flat faces keep crisp bands.
fn band3(d: f32, hi: f32, lo: f32, soft: f32, light: vec3f, mid: vec3f, dark: vec3f) -> vec3f {
    let a = smoothstep(hi - soft, hi + soft, d);
    let b = smoothstep(lo - soft, lo + soft, d);
    return select(mix(dark, mid, b), mix(mid, light, a), a > 0.0);
}

// ---- The studio, after step_viewer ----------------------------------------------------------
// Fixed to the camera, in view space with y up: a soft sky over a darker floor, light walls to
// either side, a key box above and to the right, a weaker fill to the left, a bright horizon strip
// and tall strip lights, which together make curved metal read as metal.

const kKey = vec3f(0.4537, 0.7059, 0.5440);
const kFill = vec3f(-0.7526, 0.1003, 0.6508);

// smoothstep that also runs downhill, when edge0 > edge1.
fn ramp(edge0: f32, edge1: f32, x: f32) -> f32 {
    let t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

fn studio(d: vec3f, rough: f32) -> vec3f {
    let spread = mix(0.05, 0.55, rough);
    let edge = mix(1.0, 0.3, style.sharpness);
    var c = mix(style.floorColor, style.sky, ramp(-0.35, 0.65, d.y));
    c += style.fillLight * ramp(0.2, 0.9, abs(d.x)) * select(0.6, 0.8, d.x > 0.0);
    c *= 1.0 - 0.5 * ramp(0.15, 0.95, d.z) * (1.0 - abs(d.x));
    let key = ramp(1.0 - spread * (0.9 + 0.8 * edge), 1.0 - spread * (0.9 - 0.75 * edge),
                   dot(d, kKey));
    let fill = ramp(1.0 - 2.2 * spread, 1.0 - 2.2 * spread * (1.0 - edge * 0.9), dot(d, kFill));
    let q = (d.y - 0.16) / (0.06 + 0.32 * rough);
    let strip = exp(-q * q);
    let stripWidth = 0.07 + 0.3 * rough;
    let stripEdge = stripWidth * mix(0.9, 0.25, style.sharpness);
    let tall = ramp(0.98, 0.8, abs(d.y));
    let right = ramp(stripWidth + stripEdge, stripWidth - stripEdge, abs(d.x - 0.62)) * tall;
    let left = ramp(stripWidth + stripEdge, stripWidth - stripEdge, abs(d.x + 0.7)) * tall;
    let white = vec3f(1.0, 0.99, 0.97);
    return c + style.keyLight * key + style.fillLight * fill + style.band * strip +
           white * (style.strips.x * right + style.strips.y * left);
}

// Linear light to display values: exposure, then a gentle shoulder or a filmic curve.
fn tone(color: vec3f) -> vec3f {
    let c = color * style.exposure;
    let shoulder = c / (1.0 + 0.22 * c);
    let film = saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    return pow(max(mix(shoulder, film, style.contrast), vec3f(0.0)), vec3f(1.0 / 2.2));
}

fn irradiance(n: vec3f) -> vec3f {
    let ambient = mix(style.floorColor, style.sky, 0.5 + 0.5 * n.y);
    let wrap = max(0.0, (dot(n, kKey) + 0.35) / 1.35);
    return 0.75 * ambient + 0.4 * style.keyLight * wrap * wrap +
           0.15 * style.fillLight * max(dot(n, kFill), 0.0);
}

// A surface of base colour, metalness and roughness under the studio, in display values.
fn lit(n: vec3f, v: vec3f, baseColor: vec3f, metal: f32, rough: f32) -> vec3f {
    let base = pow(baseColor, vec3f(2.2));
    let nv = saturate(dot(n, v));
    let f0 = mix(vec3f(0.04), base, metal);
    let fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0) * (1.0 - 0.75 * rough);
    let specular = studio(reflect(-v, n), rough) * fresnel;
    // Metals keep a little body colour, as a satin finish does.
    let diffuse = base * irradiance(n) * mix(1.0, 0.4, metal);
    return tone(diffuse + specular);
}

// The direction toward the eye at a pixel, in view space.
fn toEye(pixel: vec2f) -> vec3f {
    if (frame.perspective == 0u) {
        return vec3f(0.0, 0.0, 1.0);
    }
    let ndc = vec2f(pixel.x / frame.viewport.x * 2.0 - 1.0, 1.0 - pixel.y / frame.viewport.y * 2.0);
    return normalize(vec3f(-ndc.x * frame.tanHalfX, -ndc.y * frame.tanHalfY, 1.0));
}

// ---- Section caps, after step_viewer ---------------------------------------------------------
// A back face seen through a cut is exactly where the view ray crossed the plane inside a closed
// part, so it is shaded as the cut surface, hatched in the plane's own coordinates.

struct Ray {
    origin: vec3f,
    direction: vec3f,
}

// The view ray through a pixel, in world space.
fn viewRay(pixel: vec2f) -> Ray {
    let ndc = vec2f(pixel.x / frame.viewport.x * 2.0 - 1.0, 1.0 - pixel.y / frame.viewport.y * 2.0);
    if (frame.perspective != 0u) {
        return Ray(frame.eye, normalize(frame.forward + frame.right * ndc.x * frame.tanHalfX +
                                        frame.up * ndc.y * frame.tanHalfY));
    }
    return Ray(frame.eye + frame.right * ndc.x * frame.halfWidth +
               frame.up * ndc.y * frame.halfHeight, frame.forward);
}

// Coverage of one family of hatch lines, `q` counting lines. Lines crowded closer than a few
// pixels fade out rather than turn to moiré.
fn hatchLines(q: f32, pixelsPerPoint: f32) -> f32 {
    let perPixel = max(length(vec2f(dpdx(q), dpdy(q))), 1e-6);
    let pixels = abs(fract(q) - 0.5) / perPixel;
    let crowding = smoothstep(3.5 * pixelsPerPoint, 5.5 * pixelsPerPoint, 1.0 / perPixel);
    return crowding * saturate(0.4 * pixelsPerPoint + 0.5 - pixels);
}

// Hatch coverage at a pixel, on the plane the view ray entered the kept region through: 45 degree
// lines about 0.8 points wide. Each body's tag varies their direction and spacing, and crosses
// some, so cut parts side by side differ.
fn capHatch(pixel: vec2f, tag: u32) -> f32 {
    let ray = viewRay(pixel);
    var entry = 0u;
    var entryT = -3.4e38;
    var entered = false;
    for (var k = 0u; k < frame.sectionPlaneCount; k++) {
        let plane = frame.sectionPlanes[k];
        let denominator = dot(plane.xyz, ray.direction);
        if (denominator < 0.0) { // from the cut side into the kept side
            let t = -(dot(plane.xyz, ray.origin) + plane.w) / denominator;
            if (t > entryT) {
                entryT = t;
                entry = k;
                entered = true;
            }
        }
    }
    let normal = frame.sectionPlanes[entry].xyz;
    let t = select(0.0, entryT, entered);
    let world = ray.origin + ray.direction * t;
    let u = normalize(cross(normal, select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 0.0, 1.0),
                                           abs(normal.z) < 0.9)));
    let v = cross(normal, u);
    var depth = 1.0;
    if (frame.perspective != 0u) {
        depth = max(dot(world - frame.eye, frame.forward), 1e-6);
    }
    let pixelWorld = frame.worldPerPixel * depth;
    let sense = select(-1.0, 1.0, (tag & 1u) != 0u);
    let spacing = (6.0 + 2.0 * f32(tag % 3u)) * style.pixelsPerPoint * pixelWorld * 1.41421356;
    let a = dot(world, u);
    let b = dot(world, v);
    let lines = hatchLines((a + sense * b) / spacing, style.pixelsPerPoint);
    let crossing = hatchLines((a - sense * b) / spacing, style.pixelsPerPoint);
    return max(lines, select(0.0, crossing, tag % 4u == 0u));
}

// ---- Shading ---------------------------------------------------------------------------------

struct SurfaceOutput {
    @location(0) color: vec4f,
    @location(1) id: u32,
    @location(2) depth: f32, // at the pixel's centre, as Metal's depth copy is
    @location(4) tags: vec2u, // the pipeline's write mask picks solid or ghost
}

// The fragment's depth at the pixel's centre. Metal shades once per pixel, there, for all four
// samples; a jittered sample moves its depth back along the surface.
fn centreDepth(position: vec4f) -> f32 {
    let toCentre = -frame.sampleOffset;
    return position.z + dpdx(position.z) * toCentre.x + dpdy(position.z) * toCentre.y;
}

@fragment
fn surfaceFragment(in: SurfaceVertex, @builtin(front_facing) frontFacing: bool) -> SurfaceOutput {
    if (kSection && sectionCuts(in.world)) {
        discard;
    }
    let depth = centreDepth(in.position);
    let flags = in.flags & 0x77u; // state, without the shell and open bits
    var tag = (in.flags >> 8u) & 0xFFu;
    // Computed before any branch, since hatch width comes from screen derivatives.
    var hatch = 0.0;
    if (kSection) {
        hatch = capHatch(in.position.xy, tag);
    }
    let look = in.flags >> 16u;
    let selected = (flags & kStateSelected) != 0u;
    var texels = vec3u(palette[look * 3u], palette[look * 3u + 1u], palette[look * 3u + 2u]);
    if (selected) {
        texels = style.selection.xyz;
    }
    let t0 = texelColor(texels.x);
    let t1 = texelColor(texels.y);
    let t2 = texelColor(texels.z);
    let n = normalize(in.normal);

    // The inside of an open surface, seen from behind, is a little darker than its outside.
    var color: vec3f;
    if (kPhysical) {
        let v = toEye(in.position.xy);
        color = select(0.82 * lit(-n, v, t0, t1.x, t1.y), lit(n, v, t0, t1.x, t1.y), frontFacing);
    } else {
        // Derivatives are taken before choosing a side, where every pixel of a quad runs them.
        let outside = dot(n, style.lightExterior);
        let inside = dot(-n, style.lightInterior);
        let softOutside = clamp(1.5 * fwidth(outside), 0.02, 0.18);
        let softInside = clamp(1.5 * fwidth(inside), 0.02, 0.18);
        color = select(band3(inside, 0.6, 0.05, softInside, t0, t1, t2),
                       band3(outside, -0.15, -0.45, softOutside, t0, t1, t2), frontFacing);
    }

    // A cut through a closed part: the cap, hatched, and tagged so outlines mark the cut.
    if (kSection && !frontFacing && (in.flags & kFlagOpen) == 0u && frame.sectionPlaneCount > 0u) {
        var fill = t0;
        var line = mix(t2, style.ink, max(style.capInk, 0.3));
        if (kPhysical) {
            fill = t2;
            line = mix(t2 * 0.6, style.ink, max(style.capInk, 0.15));
        }
        color = mix(fill, line, hatch);
        tag |= kTagCap;
    }

    let hovered = in.id >= frame.hoverFirst && in.id < frame.hoverEnd;
    if (kShellLayer == 1u) {
        // The shell's far walls lie under everything, over the sheet they are mixed into.
        let wall = mix(color, style.sheetTop, 0.55);
        return SurfaceOutput(vec4f(mix(sheetColor(in.position.xy), wall, 0.5 * frame.seeThrough),
                                   1.0), in.id, depth, vec2u(0u));
    }
    if (kShellLayer == 3u) {
        if (hovered) {
            color = mix(color, style.hover, style.hoverAmount * select(1.0, 0.5, selected));
        }
        return SurfaceOutput(vec4f(color, 1.0 - 0.8 * frame.seeThrough), in.id, depth,
                             vec2u(0u, kTagShell | tag));
    }
    if (kGhostPass) {
        // A faint skin over what lies behind, or none at all for a ghost drawn as lines only.
        // Hovered with the ghost tool held, it shows enough to be clicked back.
        color = mix(color, style.sheetTop, 0.45);
        var alpha = select(0.0, style.ghostAlpha, (style.ghostStyle & kGhostShell) != 0u);
        if (hovered) {
            color = mix(color, style.hover, style.hoverAmount);
            alpha = max(alpha, style.ghostAlpha) * 1.6;
        }
        return SurfaceOutput(vec4f(color, alpha), in.id, depth, vec2u(tag));
    }
    if (hovered) {
        // On a selection hover lifts only half as much, so the selection stays readable.
        if (frame.hoverHides != 0u) {
            color = mix(color, style.hideTint, 0.6);
        } else {
            color = mix(color, style.hover, style.hoverAmount * select(1.0, 0.5, selected));
        }
    }
    return SurfaceOutput(vec4f(color, 1.0), in.id, depth, vec2u(tag));
}

// Classifying the shell: occurrence ids only. The peel pass keeps only what lies behind the
// front surface, and belongs to another part.
struct IdOutput {
    @location(1) id: u32,
    @location(2) depth: f32,
}

@fragment
fn classifyFragment(in: SurfaceVertex) -> IdOutput {
    if (kPeel) {
        let p = vec2i(in.position.xy);
        let front = textureLoad(frontDepths, p, 0).r;
        if (textureLoad(frontIds, p, 0).r == in.id ||
            in.position.z >= front - (1e-5 * front + 1e-7)) {
            discard;
        }
    }
    return IdOutput(in.id, in.position.z);
}

// The shadow mask: coverage only.
@fragment
fn shadowFragment(in: SurfaceVertex) -> @location(0) vec4f {
    if (kSection && sectionCuts(in.world)) {
        discard;
    }
    return vec4f(1.0);
}

// The shell's nearest front faces, into depth only.
@fragment
fn depthFragment(in: SurfaceVertex) {
    if (kSection && sectionCuts(in.world)) {
        discard;
    }
}
