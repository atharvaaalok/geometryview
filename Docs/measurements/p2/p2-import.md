# P2: the event-driven importer

Measured 2026-11-07 on the M4 Max (Chrome 157, Safari 26) and the iPhone 16 Pro (in the S3 shell).
Raw reports are in `step/`. Native times are the perf gate's fastest of five.

## What P2 built

- **One scheduler, two drivers.** `Import/step/slicing.{h,cpp}` is the STEP importer's slicing as a
  portable `StepImport` (begin, jobs, finish) and `SliceScheduler` (hands out jobs, takes back
  replies and crashes, runs the failure ladder, packs each definition as its last job returns).
  - The Mac drives it with OCCT helper processes, one thread each (`runWorkerProcesses`).
  - The browser drives it from its event loop: `geometry-step-web`
    (`Import/web/step_supervisor.cpp`) is a small C API over the same code, and the page's Web
    Workers run the OCCT jobs.
- **A warm pool.** `Web/spike/step.html` keeps its OCCT workers between files. It replaces a worker
  that traps, runs out of memory, times out (120 s) or whose heap grows past `recycleMB`
  (256 MB, since WebAssembly memory never shrinks). Each worker has W2's 512 MB cap.
- **The iOS shell** bundles the page, the supervisor and the STEP fixtures, and runs it as
  `loopback:step:<query>`.

## Same results as native

Each cache file the browsers made was saved and checked natively with `gcli verify` and
`gcli stats`.

| | Faces | Failed | Triangles | Area | Edges |
|---|---|---|---|---|---|
| demo_drive | 213, same | 0 | 20,494, identical | 4e-16 | identical |
| 13S4P pack | 9,040, same | 0 | 331,520, identical | 3e-14 | identical |
| PLE120 | 3,887, same | 0 | +8.8% | 5e-6 | identical |
| hard 52.7 MB | 12,188, same | 0 | 340,646 against 340,629 | 4e-5 | one more |

Chrome and Safari agree with each other exactly, and repeat imports give identical files. PLE120's
triangle count differs for the reason W2 gave: OCCT's mesher refines wherever a value crosses a
tolerance, and WebAssembly rounds differently (no fused multiply-add, a different maths library).
The area agrees, so the meshes are as good.

## Speed

**Whole imports**, from the file's bytes in hand to the finished cache file. A cold pool is a
session's first file; a warm pool has met the OCCT code before, as it has for every file after the
first. 12 workers on the Mac, 2 on the iPhone.

| | Native | Chrome, cold | Chrome, warm | Safari, cold | Safari, warm | iPhone, cold | iPhone, warm |
|---|---|---|---|---|---|---|---|
| demo_drive | 25 ms | 216–233 ms | 32–86 ms | 494–520 ms | 103–375 ms | 971 ms | |
| 13S4P pack | 80 ms | 456–478 ms | 169–173 ms | 1.73–1.81 s | 211–215 ms | 1.40–1.70 s | 729 ms |
| PLE120 | 179 ms | 582–605 ms | 286–295 ms | 2.19–2.25 s | 333–385 ms | 2.44–2.94 s | 1.59 s |
| hard 52.7 MB | 1.73 s | 2.82–2.99 s | 2.25–2.27 s | 4.64–4.76 s | 2.48–2.49 s | 16.5–18.2 s | 16.0–16.2 s |

- **The hard file is 1.3× native in Chrome and 1.4× in Safari once the pool is warm**, and 1.7× and
  2.7× cold. Decision 6's gate (3×) passes for every warm import of the three larger files, so no
  native accelerator is needed. The 1 MB demo is dominated by fixed costs.
- **Safari's cold cost is its lazy compiling**: its first pack import takes 1.8 s, its second 0.21 s.
  Warming the pool at startup, with a small slice per worker, would move most of that before the
  first file opens (P4).
- **Small files are dominated by per-slice overhead** (posting text and payloads, waking workers),
  which matters little in absolute terms.
- **On the iPhone, the hard file finishes in 16.5–18.2 s** with 2 workers (gate: 30 s). A third
  worker does not help (18.4 s): the phone has two performance cores. A warm pool saves only about
  a second there, since the file's 614 slices warm the workers up during the first pass anyway. A
  first run straight after unlocking the phone took 32.3 s, as the phone caught up on its own work,
  and is left out.
- One Chrome run spent 999 ms in the supervisor's first step against the usual 280–290 ms, and is
  left out of the ranges above.

## Memory on the iPhone

Instruments (Activity Monitor template) during the hard file with 2 workers:

| Process | Peak |
|---|---|
| WebContent (the page, supervisor and workers) | 393 MB |
| GPU process | 53 MB |
| The shell | 18 MB |
| **Total** | **464 MB** (gate: 900 MB) |

The supervisor's heap peaks at 163–196 MB on the hard file: the file itself (52.7 MB), its index
and structure, and the finished cache file. Each worker stays at 23–33 MB.

## Recovery

Every 10th slice (or 20th) made its worker trap or exhaust its memory before meshing. The pack's
import still finished with every face and the same triangles as a clean run.

| | Recoveries | Median | Worst |
|---|---|---|---|
| Chrome, trap | 12 | 26 ms | 61 ms |
| Chrome, out of memory | 12 | 21 ms | 80 ms |
| Safari, trap | 12 | 15 ms | 27 ms |
| Safari, out of memory | 12 | 19 ms | 61 ms |
| iPhone, trap | 12 | 29 ms | 64 ms |
| iPhone, out of memory | 5 | 27 ms | 55 ms |

## The threaded packer (W1's gate)

`Tools/probe stl` on the 10M-triangle STL, native against `wasm-threads` under Node, run
alternately, four rounds each:

| | Pack | Whole import |
|---|---|---|
| Native, now | 0.59–0.63 s | 1.24–1.29 s |
| WebAssembly, W1's code | 1.92–2.01 s | 3.58–3.67 s |
| WebAssembly, now | 1.22–1.33 s | 2.37–2.51 s |

**WebAssembly pack is 36% faster, and whole STL imports went from 2.6× native to 1.9×.** The pack
stage is 2.1× of today's native. The same changes also made native pack 16% faster, so against
the native time the gate was set on (0.70 s) it is 1.8×. **Verdict: a narrow miss of 2.0× against
today's native.** What remains is WebAssembly's general compute cost. meshoptimizer's clusterizer,
most of the stage, runs 2.16× slower in WebAssembly, with no atomics or memory traffic left to
blame.

What changed, in order of what it bought:

| Change | Effect |
|---|---|
| The sample sort's sample was drawn at `i * count / n`, which overflows a 32-bit `usize` | One bucket held 5.4M of 10M triangles; the bucket sorts ran nearly serially (191 ms against 19 ms natively). Fixed in 64 bits, with the same product in `parallelRanges` |
| The pool off Apple ran one batch at a time | The packer's edge loops queued behind the meshlet loop, so edges were the critical path. Workers now share themselves among every batch in hand, the fewest-held first |
| Crease union: a compare-and-swap per link, contended on one root | Each range of triangles unites its own edges in plain memory; only edges between ranges take the atomic path. 542 to 104 ms in WebAssembly |
| Open sides read the 3 vertices of every triangle around a side's end | Each vertex lists the vertex after each of its corners; a side reads its two ends' lists in order. 896 to about 640 ms |
| Compact ids per unit by sort and binary search | A bitset over the unit's ids, ranked by popcount. Summed over threads, 4.0 to 0.6–1.0 s in WebAssembly, 1.9 to 0.3 s natively |

The STL's cache files hash the same before and after every change, natively and in WebAssembly,
and the STEP fixtures' stats are unchanged.

## Found along the way

- **The hard file's cache was not reproducible.** A definition's slices were packed in the order
  workers answered, so its vertices came in a different order each run, and where a non-manifold
  edge keeps only its first side, the kept side changed: 159,867 or 159,868 crease edges. Slices
  are now read in face order, and five imports in a row give the same cache.
- **The perf gate's two largest fixtures** moved to the S3 archive with CADBench. The gate now also
  looks for the copies in `build/fixtures`. The STL's baseline is now 1225 ms (was 1432).
- **`--chrome-check` failed its tree-button steps on the P1 build as well as P2.** The window had
  opened under the resting pointer, on the built-in display that had become the main screen. The
  pointer's tracking moved the tree's hover off the row being checked. The check now moves its
  window aside first.

## Left for later phases

- **The supervisor runs on the page's main thread.** The web runtime (P4) moves it into a worker,
  and starts the pool's workers on a small slice so Safari compiles before the first file.
- **The finished cache file is copied out of the supervisor's in-memory file system** and then
  sliced again in JavaScript. P4 streams segments as they are written, straight to the GPU, through
  the `SegmentListener` the native driver already has.
- **Files that need OCCT's whole-file read** (none of the four fixtures) are refused in the browser
  for now.
- **The Tier 0 tessellator** is still deferred (its fixture is absent).
