// The sheet behind the model: a vertical wash, an optional vignette and drafting grid, paper grain
// and the cast shadow. Drawn at the far plane after opaque surfaces, so it shades only what they
// leave bare. Ported from sheet.metal.

@group(1) @binding(0) var shadowMask: texture_2d<f32>; // half resolution
@group(1) @binding(1) var linearClamp: sampler;

struct ScreenVertex {
    @builtin(position) position: vec4f,
}

// One triangle covering the viewport, at the far plane, which is depth 0 with reversed depth.
@vertex
fn screenVertex(@builtin(vertex_index) vertexId: u32) -> ScreenVertex {
    let p = vec2f(f32((vertexId << 1u) & 2u), f32(vertexId & 2u));
    return ScreenVertex(vec4f(p * 2.0 - 1.0, 0.0, 1.0));
}

fn maskAt(at: vec2f) -> f32 {
    let size = vec2f(textureDimensions(shadowMask));
    return textureSampleLevel(shadowMask, linearClamp, at / size, 0.0).r;
}

// The shadow at a pixel, from the half-resolution mask: a crisp tent grown a little, so hairline
// gaps between parts that touch close, or for soft styles a disc of samples.
fn shadowAt(px: vec2f) -> f32 {
    let at = px * 0.5;
    if (style.shadowSoft <= 1.0) {
        var shade = 0.0;
        for (var y = -1; y <= 1; y++) {
            for (var x = -1; x <= 1; x++) {
                let w = select(1.0, 2.0, x == 0) * select(1.0, 2.0, y == 0);
                shade += w * maskAt(at + vec2f(f32(x), f32(y)) * 0.5);
            }
        }
        return saturate(shade / 16.0 * 1.25);
    }
    // A golden-angle spiral, weighted toward the middle.
    var shade = 0.0;
    var total = 0.0;
    let radius = style.shadowSoft * 0.5;
    for (var i = 0; i < 24; i++) {
        let r = sqrt((f32(i) + 0.5) / 24.0);
        let a = f32(i) * 2.39996;
        let w = 1.0 - 0.6 * r;
        shade += w * maskAt(at + vec2f(cos(a), sin(a)) * r * radius);
        total += w;
    }
    return shade / total;
}

@fragment
fn sheetFragment(in: ScreenVertex) -> @location(0) vec4f {
    let px = in.position.xy;
    var color = sheetColor(px);
    if (style.shadowOn != 0u && frame.shadowVisibility > 0.0) {
        color = mix(color, style.shadowColor,
                    style.shadowAlpha * frame.shadowVisibility * shadowAt(px));
    }
    return vec4f(color, 1.0);
}
