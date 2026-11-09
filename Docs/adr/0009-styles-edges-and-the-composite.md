# ADR 0009: Styles as data, feature edges from topology, ink in the tile resolve

Status: accepted. Spike 8 passes on Retina displays. **Spike 10 fails its target**, and the cost
it measures is affordable for now; see below.

## Decisions

**Styles and colour modes are data.** The five styles (Illustration, Metal, Clay, Blueprint,
Technical) and four colour modes (Material, Parts, Mono, File) follow step_viewer's. A style is one
table row in `render/style.cpp` that becomes a block of shader uniforms. A colour mode decides a
*look* for each body: a material read from its name, a place in a colour cycle, a file colour or a
neutral. Looks are deduplicated, so the GPU holds three palette texels per distinct look and a
16-bit look index per occurrence. A style change rewrites a few hundred bytes. A colour mode change
rewrites the indices, 2 MB for a million parts. Flat bands and the physically lit studio are
function constants, so neither pipeline carries the other's branches.

**Feature edges come from mesh topology, at import.** The packer finds them for STEP and mesh
files alike, without asking OCCT. Only triangle sides that no other triangle shares by vertex index
can be on an edge, because sides inside a face share their vertices. Those sides are welded by
their packed grid positions, exactly what the GPU draws. An edge is classified by the normals on
its two sides:
- **Crease**: the normals differ by more than 15 degrees.
- **Smooth**: they agree. Inked only around a highlighted face.
- **Seam**: they agree and both sides are one face. Dropped.
- **Boundary**: there is only one side.

Segments are chained, so dashes run on across them, and stored in chunks of 32 with bounds, in
cache format version 4.

**Edges are screen-space quads in one mesh threadgroup per chunk.** Coverage goes to a memoryless
ink attachment with MAX blending, so quads that overlap at a joint never ink twice. Each segment
reads its two faces' state, so ghosts get dashed lines, 4 points on and 3 off along the true arc
length, and hovered or selected faces get a heavier highlight.

**Ink is applied in the tile resolve that ends the main pass.** That dispatch already reads every
attachment to copy sample 0 out for picking. It now also inks every sample. A ghost's tag in front
of a sample dims the creases behind it, as the ghost's skin would have. The overlay, drawn before
the resolve, takes away the ink beneath it.

**Outlines come from 8-bit body tags.** Each surface writes `1 + occurrence mod 255` for its body
to a memoryless attachment: red for solid bodies, green for ghosts in front of them. A composite
pass after the main pass searches each pixel's neighbourhood for:
- **Silhouettes**: a tag meets the background.
- **Part lines**: two tags meet.
- **Folds**: a step in depth within one body.
- **Ghost outlines**: the same search on the green channel.

It blends as ink plus colour times alpha, so it needs no copy of the colour. Ambient occlusion and
print texture go through the same alpha. Two bodies whose indices differ by a multiple of 255 share
a tag, and an outline between them is lost if they touch.

**Ambient occlusion and the cast shadow run at half resolution.** AO is a compute pass over the
resolved depth, run only in Metal and Clay. The shadow is the surface pass again, specialised to
flatten solid faces onto the ground along a light that follows the camera. The sheet then samples
it with a tent or a disc.

## The 120 ms finding

On a rack of a million box parts, adding edges took the GPU from 8 ms to 120 ms per frame. Edges
alone cost about 1 ms, and so did the ink apply alone; together they cost 112 ms more. The cause
was a full-screen draw that read the ink attachment back, after a million meshlets in the same
pass:

| Ink applied by | GPU per frame, 1M parts, Illustration |
|---|---|
| Full-screen draw reading the ink attachment, mid-pass | 120 ms |
| Tile dispatch, mid-pass | 16.3 ms |
| The tile resolve that ends the pass | **11.8 ms** |
| No edges at all | 10.6 ms |

The cause on the GPU was not investigated further. **Rule:** in the main pass, nothing may read
tile memory between the geometry and the resolve.

## Results

Release build, built-in display at 120 Hz, 2880×1800 pixels. GPU time is the mean per frame from
Metal's commit feedback over a scripted drag, hover and click.

| Fixture | Style | fps | GPU per frame |
|---|---|---|---|
| 13S4P pack, 837 parts | Illustration | 119.3 | 4.2 ms |
| Same | Metal (with AO) | 119.2 | 3.8 ms |
| Same | Technical | 119.3 | 4.3 ms |
| Rack of 100k parts | Illustration | 118.1 | 4.7 ms |
| Same | Technical (no shadow) | 118.0 | 4.0 ms |
| 10M-triangle STL, 1.1M crease segments | Illustration | 119.0 | 6.75 ms |
| Same, edges left out | Illustration | 119.3 | 5.1 ms |
| Rack of 1M parts | Illustration | 89.8 | 11.8 ms |
| Same | Technical (no shadow), edges left out | 119.3 | 7.9 ms |

Other measures:
- **Import**: finding edges adds 0.1 s to the 10M-triangle STL, which now takes 1.5 to 1.6 s in
  all. A large mesh finds its edges while its meshlets are built. The hard 52.7 MB STEP file
  spends 58 ms on edges.
- **Memory**: the new targets add 13 MB at 2880×1800: tags (RG8) plus half-resolution occlusion
  and shadow (R8). The ink attachment is memoryless.
- **Allocations**: zero per frame, under Metal API and shader validation.

**The shadow pass costs about 2.8 ms at a million parts**, a second pass over all geometry. That
makes 90 fps where M3 had 118 in the default style. Below 100k parts it does not show. A level of
detail for the shadow is left to M8.

## Spikes

**Spike 8, ink quality from sample-0 data: passes on Retina.** Outlines come from single-sample
tags and depth, with coverage from distance, so their position is quantised to whole pixels. At 2x
this is not visible in the golden images or the app. It was not checked on the 1x external display,
where shallow lines may show steps. The fallback, jittered accumulation at idle, is not needed yet.

**Spike 10, 5M edge segments in at most 1 ms: fails.** On the 10M-triangle STL, 1.1M segments in
view cost 1.6 ms, about 7 ms for 5M if it scales linearly. The cost is affordable: that model still
runs at 119 fps. Three things keep it bounded:
- edge chunks under 3 pixels in radius on screen are left out, since they could only blot a part;
- hidden edges fail the depth test before shading;
- the edge pass runs once, not per style.

The plan's fallback is to cull lines by visibility. The cheapest form drops an edge when both of
its faces face away. That needs the other face's normal in each segment, a format change, so it is
left to M8 with the other performance gates.

## Differences from step_viewer

- **Wobble**: it moves outlines and edges, which are sampled through it, but not the fills beneath
  them. An ink line covers a fill's edge at the amplitudes offered.
- **Ghosts**: a ghost dims solid outlines and occlusion behind it by its own alpha. step_viewer
  instead sorts every layer by depth.
- **Ghost outlines while the ghost tool is held**: ghosts then write pick ids, but outlines read
  tags, so they do not change.

## Addendum: Studio, and lines a style can leave out (2026-11-08)

A sixth style, **Studio**, draws polished metal under studio lights on a dark sweep, after a
product shot the user shared. It is the physical studio with other values, plus three switches the
renderers lacked (`Docs/measurements/p8/p8-studio.md`):
- **Lines a style can leave out, and pay nothing for.** A crease width of 0 used to draw every
  crease as a faint 1 px line, since a segment's quad is half its width plus a pixel. Now the
  inked bit needs a width, the crease pass runs only while something is selected or hovered, and
  then the object stage (Metal) or the edge cull (WebGPU) keeps only those occurrences. The
  composite's outline search and fold are a pipeline variant, left out when a style has
  neither; at zero width the search adds nothing, so the variant is exact.
- **The fold, ghost lines and a metal's diffuse share are style values**, where they were
  constants (0.85, the crease width, 0.4). The five styles carry those constants, and their
  goldens are unchanged to the pixel.
- **A dark sheet is a style's property** (`geo_style_on_dark`), which the overlay, the Mac
  masthead and the web chrome read, rather than each checking for Blueprint.

Studio's frame costs a third to three fifths of Metal's: 2.72 against 4.36 ms on the rack of 100k
parts and 2.92 against 6.75–7.61 ms on the 10M-triangle STL, on the Mac at 2880×1800.
