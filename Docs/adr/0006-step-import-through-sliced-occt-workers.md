# ADR 0006: Import STEP through our own index and sliced OCCT workers

Status: accepted. Measured on the M4 Max, release build, warm file cache, 12 workers.

## Context

Planning found that OCCT spends most of a STEP import in Transfer, mainly ShapeFix, and that reading
small slices of a file in separate processes was 5–13 times faster with unmodified OCCT. M2 had to
reproduce that natively, keep OCCT's memory out of the viewer, and match OCCT's geometry.

## Decision

The import helper indexes the file itself and never links OCCT:

1. A NEON tokenizer records every statement's id, type and references, splitting the file into 8 MB
   chunks whose string state comes from counting quotes first.
2. The structure reader replaces XCAF: products, usages, placements, units, names and colours.
3. Bodies are hashed by shape, with ids renumbered and strings and whitespace ignored. Identical
   bodies become one definition.
4. Each definition's faces are ordered spatially from the file alone and cut into slices of about
   100 kB. A body that fits in one slice is sent whole, as the real solid.
5. Single-threaded `geometry-occt-worker` processes read each slice with OCCT, mesh it, and return
   triangles keyed by STEP face id. The helper packs each definition when its last slice returns, and
   streams it to the cache as a segment the viewer can show at once.

Where the slicer cannot follow a file, such as an external scope or tessellated geometry, one worker
reads the whole file instead.

### Failure ladder

A slice that crashes, times out or exceeds the memory limit is halved until one face remains. Faces
still without triangles get one retry as part of the whole body at an eighth of the tolerance, which
also meshes small faces OCCT leaves out. A worker is killed above max(4 GB, an eighth of memory),
because one legitimate face needed 2.9 GB.

### Orientation

Every closed shell is oriented by its own volume: the outer shell positive and each cavity negative.
The file's own flags cannot be trusted. Fusion 360 writes some cavities reversed twice, and on 2 of 3
such files OCCT's whole-file read added the cavity's volume instead of subtracting it. On
`00831767.step` the shells measure 0.00975 and −0.00059, and OCCT reported their sum as positive.

## Results

| Fixture | OCCT whole file (planning) | Ours | Target |
|---|---|---|---|
| demo_drive, 1.1 MB, 29 bodies | 0.09 s | **25–27 ms** | ≤20 ms |
| 13S4P pack, 29 MB, 837 bodies to 71 | 2.62 s | **85–108 ms** | ≤0.1 s |
| PLE120, 25 MB | 1.96 s | **196–209 ms** | none set |
| Hard, 52.7 MB, one body | 19.2 s | **1.78–2.19 s** | ≤1.6 s |

The hard file misses its target. 87% of its CPU time is OCCT's Transfer, mainly ShapeFix inserting
seams into cylinders, because the file has no parameter curves. Run-to-run variation was large, and
neither 16 workers nor slices of 25–200 kB clearly helped. Reducing ShapeFix is the M8 FixShape
experiment.

Tokenizing took 2 ms for demo_drive, 10 ms for the pack and 12 ms for the hard file.

### Equivalence with OCCT

`Tools/step_equivalence.py` imports each file and compares it with OCCT reading it whole at the same
tolerances: face count, area within 0.2%, volume within 1%, and bounds within 1% of the diagonal.

| Set | Files | Match |
|---|---|---|
| All CADBench files under 8 MB | 11,957 | 11,951 |
| Stress list: voids, units, assemblies, mapped items, surface models | 525 | 523 |

Median area error on a random 324 was 0.0002%, with p99 0.006%. Twenty more CADBench files differ only
in face count, because OCCT's repair split faces whose area still matches. We keep one face per STEP
entity, which keeps ids stable. Volume comparisons ignore sign, because we turn inside-out solids the
right way round and OCCT leaves them. For files with cavities only our own sign is checked, for the
reason above.

The six remaining differences are multi-slice bodies whose faces the file orients inconsistently.
OCCT reconciles each face with its neighbours inside the whole solid, and a slice holds only part of
that solid. The stress list's two are a corrupted file whose surfaces intersect themselves, and an
engine where we leave 2 faces unmeshed and OCCT leaves 8.

### Cache size

12–23 bytes per triangle, against 36.6 in step_viewer: the pack's whole cache is 1.06 MB.

## Consequences

- Placements follow ISO 10303: the transform carries item 1 onto item 2. OCCT computes the inverse
  order, which agrees when either placement is the identity, as in every fixture seen so far.
- Faces are numbered from the file, so refinement replaces a definition's triangles without changing
  any face id. Refinement re-runs the same pipeline for chosen definitions at a finer level, and its
  results are cached beside the base file.
- The worker binary is 16 MB, linked statically against the pinned OCCT.
