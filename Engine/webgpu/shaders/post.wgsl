// After the main pass: the ink resolve, ambient occlusion and the composite. Ported from
// surface.metal's resolveTile, occlusion.metal and composite.metal.
//
// The Metal renderer inks each of its four samples in tile memory, resolves them, then composites
// once from sample 0's tags and depth. Here each sample is a jittered main pass of its own: the
// resolve inks it and adds it into the accumulated colour, and the last pass rendered is sample
// 0's, whose tags and depth the occlusion and composite read.

@group(1) @binding(0) var colorTexture: texture_2d<f32>;
@group(1) @binding(1) var inkTexture: texture_2d<f32>;
@group(1) @binding(2) var tagTexture: texture_2d<u32>;
@group(1) @binding(3) var depthTexture: texture_depth_2d;
@group(1) @binding(4) var occlusionTexture: texture_2d<f32>; // half resolution
@group(1) @binding(5) var occlusionOut: texture_storage_2d<r32float, write>;

struct ScreenVertex {
    @builtin(position) position: vec4f,
}

@vertex
fn screenVertex(@builtin(vertex_index) vertexId: u32) -> ScreenVertex {
    let p = vec2f(f32((vertexId << 1u) & 2u), f32(vertexId & 2u));
    return ScreenVertex(vec4f(p * 2.0 - 1.0, 0.0, 1.0));
}

// ---- The ink resolve -----------------------------------------------------------------------

// Inks one sample's colour from its ink: creases and highlights were drawn before ghosts, so a
// ghost's skin in front, known from its tag, dims them; ghosts' own dashed lines were drawn after
// and are not dimmed. Blended into the accumulated colour at one share per sample.
@fragment
fn resolveFragment(in: ScreenVertex) -> @location(0) vec4f {
    let p = vec2i(in.position.xy);
    var color = textureLoad(colorTexture, p, 0).rgb;
    let ink = textureLoad(inkTexture, p, 0);
    let tags = textureLoad(tagTexture, p, 0).rg;
    let inkAlpha = style.inkAlpha;
    let skin = select(0.0, style.ghostAlpha, (style.ghostStyle & kGhostShell) != 0u);
    var solid = 1.0 - (1.0 - ink.r * inkAlpha) * (1.0 - ink.b);
    if ((tags.g & kTagShell) != 0u) {
        solid *= 0.8 * frame.seeThrough; // what shows through the faded shell
    } else if (tags.g != 0u) {
        solid *= 1.0 - skin;
    }
    let ghost = ink.g * style.ghostLineAlpha * inkAlpha;
    let cover = 1.0 - (1.0 - solid) * (1.0 - ghost);
    color = mix(color, style.ink, cover);
    return vec4f(color, 1.0);
}

// ---- Ambient occlusion, from depth at half resolution ----------------------------------------
// How much of the open sky above each point nearby geometry hides, so creases, gaps between parts
// and the feet of walls darken as they do under a studio light. After step_viewer's.

fn depthAt(q: vec2i) -> f32 {
    let last = vec2i(textureDimensions(depthTexture)) - 1;
    return textureLoad(depthTexture, clamp(q, vec2i(0), last), 0);
}

// A pixel's position in view space, looking down -z.
fn viewPosition(p: vec2i, depth: f32) -> vec3f {
    let ndc = vec2f((f32(p.x) + 0.5) / frame.viewport.x * 2.0 - 1.0,
                    1.0 - (f32(p.y) + 0.5) / frame.viewport.y * 2.0);
    let z = viewDepth(depth);
    var xy = ndc * vec2f(frame.halfWidth, frame.halfHeight);
    if (frame.perspective != 0u) {
        xy = ndc * vec2f(frame.tanHalfX, frame.tanHalfY) * z;
    }
    return vec3f(xy, -z);
}

// The neighbour on an axis that lies on the same surface is the one nearer in depth.
fn tangentAt(p: vec2i, center: vec3f, axis: vec2i) -> vec3f {
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

@compute @workgroup_size(8, 8)
fn ambientOcclusion(@builtin(global_invocation_id) gid: vec3u) {
    let size = textureDimensions(occlusionOut);
    if (gid.x >= size.x || gid.y >= size.y) {
        return;
    }
    let last = vec2i(textureDimensions(depthTexture)) - 1;
    let p = vec2i(gid.xy) * 2;
    let d = depthAt(p);
    let tag = textureLoad(tagTexture, clamp(p, vec2i(0), last), 0).r;
    if (d <= 0.0 || tag == 0u || (tag & kTagCap) != 0u) {
        textureStore(occlusionOut, gid.xy, vec4f(1.0)); // the sheet, only a ghost, or a cap
        return;
    }
    let center = viewPosition(p, d);
    var normal = normalize(cross(tangentAt(p, center, vec2i(1, 0)),
                                 tangentAt(p, center, vec2i(0, -1))));
    if (normal.z < 0.0) {
        normal = -normal; // toward the eye
    }
    var pixelWorld = frame.worldPerPixel;
    if (frame.perspective != 0u) {
        pixelWorld = frame.worldPerPixel * -center.z;
    }
    let px = style.pixelsPerPoint;
    let reach = clamp(frame.aoRadius / pixelWorld, 4.0 * px, 48.0 * px);
    let radius = reach * pixelWorld;
    // Interleaved gradient noise turns the spiral per pixel; the composite's blur averages it.
    let turn = 6.2831853 *
               fract(52.9829189 * fract(dot(vec2f(gid.xy) + 0.5, vec2f(0.06711056, 0.00583715))));
    let samples = frame.aoSamples;
    var occluded = 0.0;
    for (var i = 0u; i < samples; i++) {
        let t = (f32(i) + 0.5) / f32(samples);
        let angle = turn + f32(i) * 2.3999632;
        let q = p + vec2i(round(vec2f(cos(angle), sin(angle)) * t * reach));
        let dq = depthAt(q);
        if (dq <= 0.0) {
            continue;
        }
        let v = viewPosition(q, dq) - center;
        let l = length(v);
        let falloff = saturate(1.0 - (l * l) / (radius * radius));
        occluded += max(dot(normal, v) / max(l, 1e-6) - 0.05, 0.0) * falloff;
    }
    let value = pow(saturate(1.0 - 2.6 * occluded / f32(max(samples, 1u))), 1.4);
    textureStore(occlusionOut, gid.xy, vec4f(value));
}

// ---- The composite -------------------------------------------------------------------------
// Outlines from the bodies' tags and depth, and print texture on the figure. After step_viewer's.
// Outlines are heavy where a body meets the background, lighter between bodies, and where a body
// folds in front of itself, a step in depth within one body. Ghosts get a faint outline of their
// own. Everything is sampled through the hand-drawn wobble, so outlines wobble with the edges.

const kDirections = array<vec2f, 8>(
    vec2f(1.0, 0.0), vec2f(0.7071, 0.7071), vec2f(0.0, 1.0), vec2f(-0.7071, 0.7071),
    vec2f(-1.0, 0.0), vec2f(-0.7071, -0.7071), vec2f(0.0, -1.0), vec2f(0.7071, -0.7071));

// Coverage of a line `width` pixels wide whose middle is `distance` pixels away.
fn lineCover(width: f32, distance: f32) -> f32 {
    return 1.0 - smoothstep(0.5 * width - 0.5, 0.5 * width + 0.5, distance);
}

fn tagAt(q: vec2i) -> vec2u {
    let last = vec2i(textureDimensions(tagTexture)) - 1;
    return textureLoad(tagTexture, clamp(q, vec2i(0), last), 0).rg;
}

@fragment
fn compositeFragment(in: ScreenVertex) -> @location(0) vec4f {
    let px = in.position.xy;
    let p = vec2i(px + wobbleOffset(px));
    let c = tagAt(p);

    // Search outward for a change of body, solid and ghost alike.
    var solid = 0.0;
    var ghost = 0.0;
    let reach = 0.5 * max(style.silhouette, style.partLine) + 1.0;
    for (var r = 1.0; r <= reach; r += 1.0) {
        for (var k = 0u; k < 8u; k++) {
            let o = vec2i(round(kDirections[k] * r));
            let n = tagAt(p + o);
            let distance = length(vec2f(o)) - 0.5;
            // Against the background a line is as heavy as the silhouette, and so is a cut face
            // against an uncut surface, as on a drafted section. Between bodies it is the lighter
            // part line, so two cut parts side by side keep their colour between the lines.
            if (n.r != c.r) {
                let background = c.r == 0u || n.r == 0u;
                let cut = ((c.r ^ n.r) & kTagCap) != 0u;
                solid = max(solid, lineCover(select(style.partLine, style.silhouette,
                                                    background || cut), distance));
            }
            if (n.g != c.g) {
                let background = c.g == 0u || n.g == 0u;
                ghost = max(ghost, lineCover(select(style.partLine, style.silhouette, background),
                                             distance));
            }
        }
    }

    // A body folding in front of itself, such as a boss before its own plate: a step in depth
    // within one body. A step, not a kink: the second difference must be large in world terms and
    // large against the slope, so a curved surface seen edge on does not ink its facets.
    if (c.r != 0u && (c.r & kTagCap) == 0u && solid < 1.0) {
        let s = max(1, i32(round(style.pixelsPerPoint)));
        let left = p - vec2i(s, 0);
        let right = p + vec2i(s, 0);
        let down = p + vec2i(0, s);
        let up = p - vec2i(0, s);
        let same = tagAt(left).r == c.r && tagAt(right).r == c.r && tagAt(down).r == c.r &&
                   tagAt(up).r == c.r;
        if (same) {
            let z = viewDepth(depthAt(p));
            let zl = viewDepth(depthAt(left));
            let zr = viewDepth(depthAt(right));
            let zd = viewDepth(depthAt(down));
            let zu = viewDepth(depthAt(up));
            let threshold = 10.0 * pixelWorldAt(z) * f32(s);
            let stepX = abs(zl + zr - 2.0 * z);
            let stepY = abs(zd + zu - 2.0 * z);
            let jumpX = stepX > threshold && stepX > 0.8 * 0.5 * abs(zr - zl);
            let jumpY = stepY > threshold && stepY > 0.8 * 0.5 * abs(zu - zd);
            if (jumpX || jumpY) {
                solid = max(solid, 0.85);
            }
        }
    }

    // A ghost or the faded shell in front dims the outlines behind it, as it dims everything else.
    let shellInFront = (c.g & kTagShell) != 0u;
    let layerAlpha = select(style.ghostAlpha, 1.0 - 0.8 * frame.seeThrough, shellInFront);
    if (c.g != 0u) {
        solid *= 1.0 - layerAlpha;
    }
    var layerInk = style.ghostInk;
    if (shellInFront || (c.g == 0u && ghost > 0.0 && frame.seeThrough > 0.0)) {
        layerInk = max(1.0 - 0.8 * frame.seeThrough, 0.6 * frame.seeThrough);
    }
    let ink = 1.0 - (1.0 - solid * style.inkAlpha) * (1.0 - ghost * layerInk * style.inkAlpha);

    // Ambient occlusion, blurred over the half-resolution neighbours on the same surface. A ghost
    // in front dims it.
    var shade = 1.0;
    if (style.ao > 0.0 && c.r != 0u && (c.r & kTagCap) == 0u) {
        let h = p / 2;
        let lastHalf = vec2i(textureDimensions(occlusionTexture)) - 1;
        let z = viewDepth(depthAt(p));
        let tolerance = 6.0 * pixelWorldAt(z) + 1e-5;
        var sum = 0.0;
        var total = 0.0;
        for (var y = -1; y <= 1; y++) {
            for (var x = -1; x <= 1; x++) {
                let q = clamp(h + vec2i(x, y), vec2i(0), lastHalf);
                let dq = depthAt(q * 2);
                let sameSurface = dq > 0.0 && abs(viewDepth(dq) - z) < tolerance;
                let w = select(1.0, 2.0, x == 0) * select(1.0, 2.0, y == 0) *
                        select(0.02, 1.0, sameSurface);
                sum += w * textureLoad(occlusionTexture, q, 0).r;
                total += w;
            }
        }
        shade = mix(1.0, sum / total, style.ao);
        if (c.g != 0u) {
            shade = mix(shade, 1.0, layerAlpha);
        }
    }

    // Print texture on the figure itself: a fine tooth and a slow blotch, kept faint.
    var print = 1.0;
    if (style.grain > 0.0 && (c.r != 0u || c.g != 0u || ink > 0.0)) {
        let fine = valueNoise(px / (1.35 * style.pixelsPerPoint));
        let blotch = valueNoise(px / (90.0 * style.pixelsPerPoint));
        print = mix(1.0, (0.955 + 0.07 * fine) * (0.975 + 0.05 * blotch), style.grain);
    }
    // As the Metal composite blends: ink, plus what remains of the colour beneath.
    let color = textureLoad(colorTexture, vec2i(px), 0).rgb;
    return vec4f(style.ink * ink * print + color * (1.0 - ink) * shade * print, 1.0);
}
