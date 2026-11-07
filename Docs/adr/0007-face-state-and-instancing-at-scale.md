# ADR 0007: Two-level face state, and packed batches for many small parts

Status: accepted. Measured on the M4 Max at 120 Hz, release build.

## Context

M3 adds hiding, ghosting and selection at face, body or part level, with undo, and an assembly
tree with search across several open files. Its exit criteria were a 100k-row tree at display rate,
a 1M-row spike, search within a frame, and hiding and undoing a million faces in under 1 ms.

## Decisions

**Face state in two levels.** Each occurrence has one word of flags and a page offset. Only an
occurrence with a face-level change gets a page of one byte per face; every other occurrence points
at a shared page of zeros. Hiding a part writes one word per occurrence, whatever its face count.
The renderer uploads the arrays only when their version changes.

**Hiding without discard.** The mesh stage moves every vertex of a hidden face to one point outside
the view. Faces never share vertices, so this removes exactly that face, keeps the opaque path free
of discard, and needs no per-primitive data, which ADR 0001 measured at +61%.

**Ghosts in their own pass.** Ghosts draw after everything opaque, blended, testing depth without
writing it. They write pick ids only while the ghost tool is held, so clicks normally pass through
them and a held G click restores one.

**Hover as an id range.** Occurrences are numbered depth first, so a body's or part's faces have
consecutive ids, and hovering lights one range set by a uniform, with no buffer writes.

**Packed batches.** Every occurrence used to take at least one object threadgroup. Definitions
under 16 meshlets now share batches of 32 meshlets drawn from many occurrences.

**One residency set per segment.** Each segment's set is filled and committed once and never
written again, so it can be wired on a background thread. The previous single set was being wired
in the background while the engine thread added to it, which Metal's validation layer caught.

## Results

Generated assemblies of one 4 × 4 × 8 mm box (`Tools/make_big_assembly.py`):

| | 100k parts | 1M parts |
|---|---|---|
| File | 75 MB | 774 MB |
| Tokenize | 17–48 ms | 118–128 ms |
| Read structure | 30 ms | 184 ms (was 840 ms) |
| Frame rate while dragging | 119.8 fps | 118 fps (was 68 fps before packing) |
| Footprint while rendering | 580 MB | 1.3 GB (was 2.1 GB before packing) |
| Footprint once frames stop | 361 MB | 1.1 GB |

| Check | Target | Measured |
|---|---|---|
| Hide and undo a part of 1M faces | ≤1 ms | under 1 ms (unit test) |
| Search 1M names | within a frame | 2.1–2.7 ms, split across cores |
| 10M-triangle STL | display rate | 119.3 fps, 553 MB after interaction (760 MB at M1) |

## What is not understood

The million-part scene still holds about 1 GB of memory the system attributes to the process as
"owned physical footprint (unmapped)" once it renders continuously. It does not grow with time, does
not follow the payload size (halving it changed nothing), and grows with the number of draws
(4,096 batches per draw used the least). It appears to be driver memory for mesh shading that
scales with the threadgroups dispatched. Finding its cause needs Metal System Trace, and is left for
M8, along with a classic instanced path for definitions small enough not to need meshlets.

## Not measured

- Scrolling the tree by hand. The table is virtualized and rows are fixed height, so its cost does
  not depend on row count, but no frame times were taken.
- Real mouse and keyboard use of hold-H and hold-G. The engine paths are covered by unit and GPU
  tests, and the live app was driven only by the scripted exercise.
