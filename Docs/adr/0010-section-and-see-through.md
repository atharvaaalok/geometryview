# ADR 0010: Sections with caps, and see-through in three layers

Status: accepted. Spikes 6 and 7 pass.

## Sections

**Decision.** Up to six planes cut the model through hardware clip distances. A clip distance
costs nothing until it cuts a triangle, so parts the planes miss pay nothing. While a section is
open, closed parts stop culling back faces. A back face seen through a cut is exactly where the
view ray crossed the plane inside the solid, so the surface shader draws it as the cap, as
step_viewer does:
- the cap is the part's fill, hatched at 45 degrees in the coordinates of the plane the ray
  entered through;
- each body's tag varies the hatch's direction and spacing, and crosses every fourth, so cut parts
  side by side differ;
- surfaces that are not closed get no cap.

Clip distances live in a function-constant member of the mesh output, so pipelines without a
section carry none. Caps set the top bit of their tag, so the composite inks a cut face against an
uncut surface as heavily as a silhouette, as on a drafted section, while two cut parts side by side
keep the lighter part line. The planes are offered as a cut across each axis, at a fraction of the
model's bounds, removing either side.

**Spike 7, caps on 20 models with one and two planes: passes.** Twenty CADBench parts, picked
evenly from the medium and hard sets, were cut at x = 0.5, and at x = 0.6 with y = 0.4 from the
other side. Every cut showed a hatched cap, with no stray caps or holes. A cap keeps the depth of
the back face it is drawn on, not of the plane. Anything really in front of the cut still hides
it, and parts inside a cut housing show in front of its far wall.

**Amended (v0.1.1): caps by count, on the plane.** In assemblies the nearest back face is not
always the cut. Where part A's far wall touches part B, A's back face and B's front face lie at
the same depth. Flat contacts, such as a cover in a housing, z-fight. Round ones, such as a shaft
in a bearing, where two faceted cylinders interleave by a facet's sag, show B in stripes that turn
with the view. Where B's faceted face reaches a hair into A, B's back face can even be the nearest.
A depth bias cannot fix the round case without breaking thin walls. Drawing every back face on the
plane is wrong too: a far wall behind its own near wall is no cut.

So, after the opaque surfaces, two more draws of the closed parts:
- **Count.** Every closed surface behind the cut, whatever its depth, toggles its body's 7-bit
  tag into a per-sample code, and the top bit counts them all. The plane at a pixel lies inside a
  part exactly when that part is crossed an odd number of times, and every other part an even
  number. So where the top bit ends set, the code is that part's tag. Counting doesn't care which
  of two touching surfaces comes first. Closed definitions are watertight by construction: every
  edge is used by two triangles, and shared positions decode alike (ADR 0004).
- **Caps.** Back faces only, drawn where the count is odd and the code is their own part's tag.
  Each writes its depth on the plane the ray entered through, less 1/1024 of the way back to its
  own depth, so the nearest of a part's back faces wins and nothing behind the cut can. The planes'
  depths across the screen are uniforms (`sectionDepths`), solved from the frame's own view
  projection. A pick on a cut face therefore lands on the plane.

Metal keeps the code in an R8Uint tile attachment, read and written by programmable blending; the
caps read it too, and write `[[depth(greater)]]`. WebGPU has neither, so a pass of its own counts
into two RGBA16Float targets by additive blending, one channel per bit, and the caps read them as
textures. Both draws come after the opaque path, so its hidden-surface removal is untouched.

**Known limits:**
- Where two closed parts overlap at the plane, as in an interference fit, both counts are odd and
  no part's code results. There the opaque pass's own caps stay, as before.
- Two parts whose tags are equal, that is whose indices differ by a multiple of 127, cannot be told
  apart by the code.
- A closed part with a face hidden or ghosted alone counts wrongly behind that face.

## See-through

**Decision.** T fades the outer shell over 0.7 seconds, after Colorfigures' layers, in one pass:

| Layer | How |
|---|---|
| Inner parts | Opaque, as usual. The opaque passes leave the shell out while it fades |
| The shell's far walls | Its back faces, opaque but mixed with the sheet at 0.5k. A viewport depth range of their own holds them just in front of the sheet, so they lie under the mechanism wherever they overlap it |
| The shell's nearest front faces | Into depth, then again blended at opacity 1 − 0.8k with depth equal and a one-shot stencil |
| Ink behind the shell | Creases, outlines and occlusion behind it are dimmed by its opacity in the tile resolve and the composite |
| The shell's outline | At the shell's opacity, but never weaker than 0.6k |

The stencil starts at zero and is incremented by the first front face to pass at each sample, so
later faces at the same depth fail. That is spike 6.

**Spike 6, double blending on coincident faces: passes.** A test draws a box whose every face has
a twin at exactly its depth, and compares it with a single box. No pixel differs. With the stencil
test disabled, 5,454 values differ, so the test does detect double blending.

**The shell is classified on the GPU.** The classification itself is step_viewer's, in
`render/shell.cpp`. A part is shell when all of these hold:
- it is mostly seen from outside;
- what it hides is mostly inner parts;
- it does a real share of that hiding;
- it encloses at least one part it hides.

Two kinds of name override this. Housings, covers and caps fade on weaker evidence. Gears, magnets
and other mechanism never fade. A second round, with the first shell left out, catches a second
layer such as a stator around its rotor.

The renderer gathers the counts. It draws 14 square views of occurrence ids at 160 pixels, then
peels each view with a pipeline variant that discards all but the nearest other part behind. The
result is the top bit of each occurrence's look index, so it costs no new buffer. On the PLE120
gearbox it takes 25 ms, once, when see-through is first asked for, and again when geometry
finishes arriving.

## Deferred

- **Per-part override.** The plan has a per-part override of the shell, with a tree action or G
  holding it. Not built yet. The classification is the only source for now.
- **See-through of every part.** step_viewer's "see-through all", six layers peeled, is not built.
  The plan lists only the shell mode.
- **The section gizmo.** The plane is not drawn in the view. The card's sliders and the cut itself
  show where it is.
- **A union of cuts.** Removing only where every plane cuts, as for a quarter cut-away, needs
  discard and is not built. The cuts offered remove everything any plane cuts.
