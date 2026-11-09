# P3: the WebGPU renderer

Measured on 2026-11-07 (evening, Pacific Time) on:
- the M4 Max, with Chrome 157, Safari 26 and native Dawn;
- the iPhone 16 Pro, in the S3 shell over loopback HTTP.

The raw browser and iPhone reports are in `renderer/`. Native numbers come from `geometryview-webgpu`:
- `compare` renders the same frame with both renderers;
- `bench` times a 240-frame orbit, waiting for each frame;
- `bench --metal` times the same orbit with the Metal renderer.

The design is in ADR 0014.

## What P3 built

`Engine/webgpu/` is a second `render::Backend` beside Metal's, in portable C++ against
`webgpu_cpp.h`. It builds natively on Dawn and to WebAssembly with Emscripten's `emdawnwebgpu`.
Its shaders are WGSL ports of the Metal ones, in `Engine/webgpu/shaders/`.

It draws everything the Mac app draws:
- the five styles and four colour modes;
- the cast shadow, ambient occlusion and the floor sheet;
- feature edges, outlines and the ink;
- sections with their caps;
- ghosts, hidden, selected and hovered states, the hide tool and see-through;
- perspective;
- the view cube and the scale bar.

It also does picking and shell classification (natively).

Around it:
- **`engine::prepareOffscreen`** (`Engine/core/offscreen_view.cpp`) sets up a headless frame for any
  backend. `gcli render`, the tests and the tools share it.
- **`geometryview-webgpu`** (`Tools/webgpu/main.mm`) compares and times the renderer on Dawn against
  Metal.
- **`geometryview-webgpu-bench`** (`Tools/webgpu/web.cpp`) is the renderer as WebAssembly.
  `Web/spike/renderer.html` drives it in the browsers and on the iPhone.
- **`Tests/unit/webgpu_test.mm`** draws every golden scene and every state with both renderers. It
  also checks picking, the overlay, shell classification, and that the WGSL uniforms are laid out
  as the C++ ones are.

## Same images as Metal

`webgpu_test.mm` runs with the unit tests on the `dawn` preset. Each image must:
- differ from Metal's by less than 1 level on average;
- have under 0.5% of its pixels more than 12 levels off.

| Test | Scenes | Result |
|---|---|---|
| Styles × colour modes, demo_drive | 14 | pass; the worst pixel in the Illustration scenes is 7 levels off |
| States | 7: section-x, section-xz-metal, ghosts, hidden-selected-hovered, hide-tool, see-through, perspective-metal | pass; largest share of changed pixels 0.05% |
| Overlay, light and dark ink | 2 | pass; worst pixel 6 levels |
| Picking: ids and depths at sample points | 1 | identical |
| Shell classification | 1 | 99.5% of front ids and 99% of behind ids match |

**Real models at 1600×1000, against the plan's gate** (no more than 0.5% of pixels more than 8 levels off):

| Scene | More than 8 off | More than 12 off | Mean | Worst |
|---|---|---|---|---|
| PLE120 | 0.003% | 0% | 0.11 | 10 |
| PLE120, sectioned at x = 0.5 | 0.268% | 0.261% | 0.28 | 219 |
| 13S4P pack, perspective | 0.013% | 0% | 0.15 | 67 |
| 13S4P pack, Blueprint | 0% | 0% | 0.004 | 4 |
| Hard file (52.7 MB STEP) | 0.010% | 0% | 0.14 | 38 |
| 10M STL | 0.266% | 0.118% | 0.08 | 97 |
| Rack of 100k parts | 0% | 0% | 0.001 | 6 |

**Verdict: the gate passes on every scene.** Where pixels still differ:
- **The sectioned PLE120:** on small interior parts inside the cut, where caps and the walls of
  neighbouring parts meet.
- **The STL:** single pixels scattered over the surface. At this size, edge ink covers most of
  that surface.

Neither has been traced further.

## Speed

### Native, Dawn against Metal, 2880×1800

GPU time per frame (p50) over the same orbit. The machine's load average was 4.3–4.8. Both
renderers wait for each frame, so the GPU clocks are lower than in a paced app.

| | WebGPU, 1 sample | WebGPU, 4 jittered samples | Metal, 4× MSAA | Spike (Phase 0), 1 sample |
|---|---|---|---|---|
| 10M STL | 5.29 ms | 10.46 ms | 7.55 ms | 5.05 ms |
| 13S4P pack | 1.21 ms | 2.04 ms | 1.72 ms | 1.5 ms |
| Rack of 1M parts | 8.28 ms | 16.01 ms | 19.35 ms | 5.62 ms |
| Rack of 100k parts | 3.94 ms | 10.49 ms | 4.37 ms | 3.27 ms |

- **In motion, one sample a frame is faster than Metal on every scene**, at 0.43–0.90× of its time.
- **At equal antialiasing, it is not.** Four jittered passes cost 1.19× Metal on the pack, 1.39× on
  the STL and 2.4× on the 100k rack. Metal's memoryless 4× MSAA shades once per pixel and redraws
  nothing, which WebGPU cannot match.
- **That settles decision 8: the Mac keeps the Metal app** (see ADR 0014).
- **The 1M-part rack costs 47% more than in the spike.** The spike had no states, looks, ordered
  lists or half-space cull. That cost has not been profiled yet.
- Metal's offscreen rack (19.35 ms) is slower than ADR 0009's 11.8 ms in the app at 120 Hz. The
  difference is the waiting and the low clocks of the bench.

**Edges cost about 1.6 ms of the STL's 5.3 ms.** They are expanded per vertex, six vertices to a
segment, from the cache's segment records. The plan's alternative, a compute pass that sets up each
segment first, was not built. Its comparison stays open, against this measured 1.6 ms.

### Browsers on the Mac, 2880×1800, 1 sample a frame

A 6 s orbit at each of three zooms. "Capacity" draws 4 frames per display refresh, so the GPU
stays busy. "Paced" draws one a refresh; here the display ran at 60 Hz.

| GPU ms (p50 / p99) | Zoom 1 | Zoom 2 | Zoom 4 |
|---|---|---|---|
| Chrome, STL, capacity | 5.31 / 6.03 | 4.33 / 5.70 | 3.02 / 4.33 |
| Safari, STL, capacity | 5.78 / 6.94 | 4.53 / 5.02 | 3.11 / 4.26 |
| Chrome, pack, capacity | 2.82 / 5.90 | 3.21 / 4.00 | 3.21 / 3.93 |
| Safari, pack, capacity | 2.95 / 5.61 | 2.92 / 6.27 | 2.85 / 5.59 |
| Chrome, STL, paced | 10.88 / 14.09 | 9.83 / 12.45 | 7.08 / 12.26 |
| Safari, STL, paced | 7.18 / 12.77 | 7.90 / 17.07 | 9.59 / 12.98 |

- **The gate (Chrome, STL, p50 ≤10.1 ms, p99 ≤16.7 ms) passes at capacity, at about half the
  limit**: 5.3 ms, the spike's 5.05 ms plus everything P3 added.
- **Paced at 60 Hz, Chrome's zoom-1 p50 is 10.9 ms.** That is the clock governor taking the slack
  of a 16.7 ms frame: the frame intervals hold at 16.7 ms (p99 17.7 ms), and no frame is dropped.
- **Loading the STL's cache file into the renderer** takes 110–114 ms in Chrome and 41–43 ms in
  Safari. That covers copying it into WebAssembly, building the scene and uploading it.
- **Hover probes:** every probe came back (1,158–3,535 per run, none failed).

### iPhone 16 Pro, DPR 2 (804×1748) unless noted, 1 sample a frame, paced at 60 Hz

| GPU ms (p50 / p99) | Zoom 1 | Zoom 2 | Zoom 4 |
|---|---|---|---|
| Pack | 8.56 / 15.55 | 8.46 / 16.03 | 8.62 / 13.82 |
| Pack, capacity (3 frames a refresh) | 3.98 / 4.07 | 3.89 / 4.39 | 3.47 / 5.66 |
| Pack, DPR 3 (1206×2622), thermal state "fair" | 8.78 / 15.10 | 8.65 / 14.96 | 10.34 / 23.87 |
| STL, full detail | 30.65 / 33.44 | 27.74 / 32.19 | 11.82 / 17.57 |
| STL, reduced detail (no edges or shadow) | 12.82 / 13.72 | 12.10 / 13.53 | 9.42 / 14.19 |

- **The pack's gate (p99 ≤16.7 ms at DPR 2) passes** at every zoom, with every hover probe hitting.
  Its capacity is about 4 ms a frame.
- **At DPR 3 the closest zoom misses** (p99 23.9 ms), with the phone warm. The web runtime should
  render at DPR 2 on phones.
- **The STL at full detail runs at about 30 fps.** With reduced detail, as the Mac app uses for
  heavy scenes in motion, it holds 60 fps (p99 13.5–14.2 ms). The full image is drawn once the
  view settles.

## Memory

**GPU memory the renderer holds**, as `Renderer::memoryBytes` counts it:
- **Buffers:** 268.8 MB for the STL, 5.6 MB for the pack, 363.4 MB for the 1M-part rack.
- **Render targets:** 35.25 bytes per pixel when the reports were taken (174.3 MB at 2880×1800;
  47 MB at the iPhone's DPR 2).
  - At full resolution: colour, ids, depth copy, ink and depth at 4 bytes each, tags at 2,
    accumulation at 8, and an offscreen copy at 4.
  - At half resolution: the shadow mask at 1 byte and occlusion at 4.
- **Since commit `63f737e`**, the offscreen copy is made only for snapshots. Targets are now
  31.25 bytes per pixel, 154.5 MB at 2880×1800.

**The iPhone**, from Instruments' Activity Monitor template, with the STL open:

| Process | P3 | Phase 0 spike |
|---|---|---|
| WebContent (page and WebAssembly) | 938 MB | 868 MB |
| GPU process | 264 MB | 226 MB |
| **Total** | **1,202 MB** | **1,094 MB** (gate: 900 MB) |

**The gate still fails, by the same cause as in Phase 0.** The bench page holds the 151 MB file
three times on the CPU:
- as fetched;
- copied into the WebAssembly heap (363 MB in all);
- as the segment the renderer uploads from.

P4 streams the cache from fetch or OPFS straight into GPU buffers. That removes those copies, about
450 MB. The renderer's own share is its 269 MB of buffers and 47 MB of targets. The pack uses 16 MB
of WebAssembly heap.

Instruments now loses the phone about a second into each recording ("Device disconnected"), so the
figures are each recording's opening snapshot, taken with the orbit running.

## The Mac app

P3 changed three Metal shaders and moved headless setup into `engine::prepareOffscreen`.

- **Unit tests** pass in debug, ASan and TSan.
- **`--chrome-check`** passes.
- **`--exercise` on the pack** holds 59.7–60.0 fps on the 60 Hz built-in display, and chrome-only
  changes draw no 3D frame.
- **Allocations:** it reports 52 allocations in its last frame, exactly as `main` does in
  alternating runs. The count covers only the Metal renderer's `render` call.
  - Builds of the Phase 0 merge (`135c46c`) and of refinement round 2 (`38326e9`) report 54. So this
    predates the universal viewer.
  - ADR 0013's "0 allocations per frame" was wrong.
  - Where they come from is not yet traced. PLAN.md asks for zero.
- **`Tools/perf_gate.py`** passed four fixtures. It reported the STL 6–7% over its baseline, at a
  load average of 7–10.
  - P3 changes no import code, and its release flags are unchanged.
  - Six alternating imports each of `main` and P3 gave the same fastest time (1.307 s and 1.298 s)
    and byte-identical cache files.

## Found along the way

**The Mac app.** Comparing against Metal found two bugs in the Mac app and one waste, all fixed in
its shaders:
- **Perspective views showed speckle in the ambient occlusion and outlines.** It came from fast math
  where depth is turned back into positions. A precise division alone did not cure it.
  `occlusion.metal` and `composite.metal` now use safe math.
- **Section caps and far walls went missing behind a cut.** Cone culling dropped meshlets whose
  back faces the cut, or the shell's back layer, exposes. Cone culling is now off whenever a section
  is open, and for the shell's back layer.
- **Meshlets wholly in the cut half were drawn and then discarded.** They are now culled.

**Driver and browser limits:**
- **Safari rejects a pipeline whose override constants include one a stage does not use** ("Vertex
  library failed creation"). Each stage is now given only its own.
- **More than 8 storage buffers in one stage** exceeds the default limit. The edge slots moved into
  the slot buffer, and face states are read only in the vertex stage.
- **The frame uniforms grew past 256 bytes** (to 416). Slots that were 256 bytes apart overlapped,
  which broke the shadow until the stride was computed from the struct.
- **Z-fighting changed from frame to frame**, because compaction appended meshlets in whatever
  order threads finished. Lists are now written in instance order (count, scan, write).
- **Edges and the overlay were sharper than Metal's**: a jittered sample moved them by its own
  offset. Metal evaluates their varyings once, at the pixel's centre. `sampleOffset` lets the
  shaders move them back there.

## Left for later phases

- **Settled accumulation is all at once**: 4 samples in one frame. P4's frame loop should add one
  sample per frame while the view is still, and submit nothing once it has four.
- **The shadow is drawn from the full scene.** A coarser level would save much of its cost on the
  STL. Reduced detail drops it in motion for now.
- **Shell classification blocks on its readback**, so the browser build does not have it. P4's
  asynchronous engine path needs it.
- **The iPhone memory gate** moves to P4's streaming.
- **The rack's cull cost** needs profiling: 8.3 ms against the spike's 5.6 ms.
- **The overlay's text** comes from the host. The Mac draws it with Core Text. The web runtime draws
  it with Canvas2D (P4).
