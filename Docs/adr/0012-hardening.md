# ADR 0012: Hardening: shape healing, fuzzing, the sandbox, performance gates and spikes 2 and 5

Status: accepted. Some of M8 is deferred, with the reasons below.

## Shape healing stays at OCCT's default

The hard 52.7 MB file is the one fixture that misses its import target: 1.6 s wanted, 1.8 to 2.2 s
measured. Its workers spend 15 s of CPU in transfer, nearly all of it in ShapeFix. In OCCT 8,
STEP's default shape processing is the single FixShape operation. Its sub-modes can be set on the
reader, but only after a file is read, since the actor that holds them exists only then.
`GEOMETRY_SHAPE_PROCESS` selects the variants measured:

| Healing | Transfer CPU | Import | Hard file's geometry | Equivalence |
|---|---|---|---|---|
| Default (FixShape) | 15.0 s | 1.83 s | 340,629 triangles, area 8298.51 | as before |
| None | 0.6 s | 1.84 s | 1,016 triangles, 2% of the area: most faces fail to mesh | not run |
| Lean: no intersecting edge and wire checks | 9.3 s | **1.33 s** | 340,613 triangles, area within 0.0005% | **497 / 499** |

The equivalence sample is every 24th file of CADBench. Both files that lean healing breaks match
with default healing:
- One loses 2 of 6 faces. A retry with full healing could catch this.
- One loses 0.24% of its area with no failed faces, which nothing would catch.

Turning off fewer checks does not save these files, because the intersection checks they need are
where the time goes.

**Decision: keep the default.** Leaving roughly one file in 250 quietly wrong is worse than 0.4 s
on the slowest fixture. The switch stays, for experiments.

## Tier 0 tessellator: deferred

Tier 0 means meshing planar faces with straight edges, faceted B-reps and tessellated STEP without
OCCT. Its target was the 792 MB file from planning, which is not on this machine, so the target
cannot be measured. The largest STEP files present, 538 to 643 MB, are B-spline surfaces Tier 0
would not take. Among the fixtures, a scan of planes bounded only by lines finds:

| Fixture | Faces | Planar | Planar with line edges |
|---|---|---|---|
| Hard 52.7 MB | 12,188 | 3% | 3% |
| 13S4P pack | 9,040 | 68% | 44% |

The pack already imports in about 0.1 s thanks to deduplication, and Tier 0 would barely touch the
hard file. It is deferred until a fixture that needs it is at hand.

## Fuzzing

`Tools/fuzz.sh` builds three libFuzzer targets with ASan and UBSan, seeded with real inputs.

| Target | Runs | Time | Failures |
|---|---|---|---|
| STEP tokenizer and parameter cursor | 1,443,005 | 600 s | 0 |
| STL reader, binary and ASCII | 25,792,183 | 600 s | 0 |
| Cache segment validation, then walking what validates | 40,928,704 | 600 s | 0 |

The plan's 24 hours per target were not run; ten minutes each is what this session afforded. The
script takes a time in seconds for longer runs.

## Sandbox review

| Binary | Entitlements | What it reaches |
|---|---|---|
| Geometry.app | sandbox, user-selected files read-only, app-scoped bookmarks | Files and folders the user chose, through security-scoped URLs. Connected folders through bookmarks. Its own container, which holds the cache |
| geometry-import | sandbox, inherit | Nothing by path. The source file arrives as an open descriptor and the cache file is written to a descriptor, so the helper that parses untrusted files names no file itself |
| geometry-occt-worker | sandbox, inherit | Requests and slices over fixed descriptors 3, 4 and 5, so OCCT writing to stdout cannot corrupt the protocol |

The viewer treats every cache file as untrusted, since a compromised helper could write one:
- sections are validated on read;
- shaders clamp vertex, face and segment indices that would cost too much to check;
- the cache reader is one of the fuzz targets above.

Workers run under a memory limit of max(4 GB, RAM/8). The supervisor holds no OCCT.

## Performance gates

`Tools/perf_gate.py` imports each fixture five times, cold, and fails when the fastest of the runs is
over 5% (and 10 ms) above `Tools/perf_baseline.json`. The fastest run, not the median, because
medians of back-to-back runs moved by up to 7% for the multi-process STEP imports. Twice in a row,
the gate then passed within 3% of baseline:

| Fixture | Baseline, fastest of 5 |
|---|---|
| demo_drive | 25 ms |
| pack_13S4P | 82 ms |
| ple120 | 192 ms |
| hard_52mb | 1758 ms |
| stl_10m | 1432 ms |

Frame times are gated by the exercise and ADRs 0005 and 0009, not by this script. It runs only
here, on AC power, since the fixtures live outside the repository.

## Spikes

**Spike 2, the mesh shader path on M1 or M2: not run.** No such hardware was available.

**Spike 5, 30M triangles for 60 s: passes, once heavy scenes shed detail in motion.** The scene was
three copies of the 10M-triangle STL, coinciding, dragged for 60 s on AC power.

| Configuration | fps | GPU mean | GPU p99 |
|---|---|---|---|
| Illustration, everything drawn | 48.2 | 29.1 ms | 47.9 ms |
| Technical, no shadow | 47.7 | 21.0 ms | 30.5 ms |
| Technical, no shadow, no edges | 119.4 | 5.5 ms | 6.6 ms |
| **Illustration, edges and shadow left out while moving** | **119.9** | **5.4 ms** | **6.5 ms** |

The 3.3M coinciding edge segments cost about 15 ms and the shadow about 8 ms. The plan's fallback
was level of detail, then occlusion culling. The detail shed is temporal instead. A scene of over
20M triangles or 2M edge segments, counting every instance, leaves edges and shadow out while the
view moves, and draws them in the first frame after it settles, which the refinement timer already
marks. Battery power was not tested.

## Still open

- **Driver memory at a million parts.** ADR 0007 found about 1 GB of driver-owned memory. It was
  not investigated further.
- **Spike 10's fallback.** Dropping edges whose faces both face away needs the other face's normal
  in each segment, a format change.
- **The shadow pass's cost at a million parts.** 2.8 ms, which wants a level of detail.
