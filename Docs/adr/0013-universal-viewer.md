# ADR 0013: The universal viewer: what Phase 0 measured and decided

Status: accepted (2026-11-07). The build phases P1–P7 follow it.

## Context

**The goal:**
- one viewer for iPhone, Mac, Windows and Linux, local only, with no hosted site and no accounts;
- easy to plug into AI apps;
- as fast as possible, speed first and memory second.

**The plan of 2026-11-07:**
- the C++ engine compiled to WebAssembly, with a lean WebGPU renderer and a framework-free interface;
- a Node launcher that serves it on 127.0.0.1 and is the MCP server for AI apps;
- a thin WKWebView shell on iPhone;
- the native Metal app kept for the Mac.

Phase 0 measured the risky parts against go/no-go gates before any of that is built. All of its work is on branch `universal-phase0`. The Mac app is unchanged: see "The Mac app" below.

**Phase 0's reports:**

| Spike | Report |
|---|---|
| S0, capability census | `Docs/measurements/phase0/census/` |
| S1, AI integration on the Mac | `Launcher/`, and README "Using it from AI apps" |
| W1, the engine core as WebAssembly | `Docs/measurements/phase0/w1-engine-core.md` |
| S3, the iOS shell | `Docs/measurements/phase0/s3-ios-shell.md` |
| R, the WebGPU renderer | `Docs/measurements/phase0/r-renderer-spike.md` |
| W2, OCCT in Web Workers | `Docs/measurements/phase0/w2-occt-workers.md` |

**Hardware:**
- an M4 Max (40-core GPU) with Chrome 157 and Safari 26;
- an iPhone 16 Pro on iOS 26.7.2.

## Gates

| Gate | Measured | Verdict |
|---|---|---|
| **S0** WebGPU in the AI apps' side panes | Claude's pane (Chrome 152) and ChatGPT's (Chrome 156): WebGPU, cross-origin isolation, OPFS, WebGPU in workers | pass |
| **S1** AI apps can open, outline and picture a model | MCP tools `open_model`, `get_structure`, `render_view`, `open_in_viewer` work from Claude Desktop (`.mcpb`) and Claude Code | pass |
| **W1** portable unit tests in WebAssembly | 149 pass single-threaded (1 skipped), 150 threaded | pass |
| **W1** simd shim against Apple's | within 1e-12 on a million cases | pass |
| **W1** cache blobs byte-identical | STEP structure identical; STL blobs differ in about 0.06% of normals by one unit | **fail, accepted**: caches are keyed per engine |
| **W1** hard file tokenizes in ≤40 ms | 37 ms threaded, 85 ms on one thread | pass |
| **W1** threaded STL packer within 2× of native | 2.7× (WebAssembly atomics in union-find and sort) | **fail**. After P2: 2.1× of today's native, 1.8× of the native time then; whole STL imports 1.9× (`p2/p2-import.md`) |
| **S3** WebGPU in WKWebView | works from geo:// and from loopback HTTP | pass |
| **S3** memory ceiling ≥1.1 GB | iOS stops the page at 1.7–2.0 GB of WebAssembly or 1.8 GB of GPU buffers | pass |
| **S3** file streaming ≥300 MB/s | 1.5–3.4 GB/s | pass |
| **R** Mac Chrome, 10M STL with edges and shadow at 2880×1800, GPU p50 ≤10.1 ms, p99 ≤16.7 ms | 5.05 ms unpaced; at 120 Hz, 5.26 ms p50 and 8.1 ms p99; Chrome's GPU time equals Dawn's | pass |
| **R** iPhone, pack, p99 ≤16.7 ms | about 15 ms paced at 60 Hz; 6.3 ms capacity even at DPR 3 | pass |
| **R** iPhone, STL, ≤900 MB and no termination over 10 minutes | never stopped and never grew; 868 MB page + 226 MB GPU process | **fail on memory**: fixes known, see P4. P3's full renderer: 938 + 264 MB. P4's viewer, streaming into GPU buffers: **pass**, at most 630 MB (390–470 MB page + 160–225 MB GPU process) |
| **R** pick readback p99 ≤2 vsyncs | under one frame: 5–11 ms on the Mac, 18–48 ms frames on the iPhone | pass |
| **R** zero submits when idle | not measured: a property of P4's frame loop, as in the Mac app | P4: **pass**. 0 frames in 3 s still, in Chrome and Safari, after one four-sample frame |
| **R** image ≤0.5% off the Metal goldens | not measured: the spike's styles approximate the Metal ones | P3: **pass**. Real models at most 0.27% of pixels more than 8 levels off; every golden scene and state tested (`p3/p3-renderer.md`) |
| **R** native Dawn against Metal | geometry pass 1.36 ms against 1.38 ms (Metal mesh shaders); whole Illustration frame faster than the Metal app's | recorded, see decision 8 |
| **W2** OCCT wasm ≤12 MB, ≤4 MB brotli | 9.7 MB, 2.4 MB | pass |
| **W2** matches native: faces, failures, area within 1e-4 | all equal; area within 3.5e-5 on the hard file | pass |
| **W2** matches native: triangles ±0.5% | 0.005% on the hard file, but +6.9% on PLE120 | **fail, accepted**: floating-point differences; area is the right test |
| **W2** per slice p50 ≤2.0×, p95 ≤3.0× | Chrome 1.33–1.37× / 1.43–1.52×; Safari 1.31–1.41× / 1.46–1.57× | pass |
| **W2** hard file ≤4 s in Chrome, ≤5 s in Safari | OCCT phase 2.5 s (Chrome), 4.45 s cold and 2.8 s warm (Safari), plus about 0.5 s for the rest | pass in Chrome; **Safari at the gate cold**, pass warm |
| **W2** trap or out-of-memory recovered in ≤100 ms | 7–14 ms in Chrome, 14–25 ms in Safari, 18–62 ms on the iPhone; no face lost | pass |
| **W2** iPhone, 2 workers, hard file ≤30 s, ≤900 MB, no termination | 16.2 s; about 390 MB; never stopped | pass |

**The Mac app** is unaffected by Phase 0:
- unit tests pass in debug, ASan and TSan;
- `--chrome-check` passes;
- `--exercise` holds 59.8 fps on a 60 Hz display. This said "with 0 allocations per frame", which was wrong: the exercise reports 52–54 in its last frame on every build back to refinement round 2 (see `p3/p3-renderer.md`);
- the importer matches `main` when the two are run alternately under the same load. Stage by stage they agree within noise: tokenize 11 ms; worker phase 1.72–1.83 s on the hard file.

`Tools/perf_gate.py` itself reported up to 14% slower than its baseline, because the machine was busy with other work at the time.

## Decisions

**1. The web renderer is its own lean product; the cache format is the contract.** Confirmed. The spike reads the cache's own records, uploaded unchanged, so Mac and web share one importer and one cache format.

**2. The pass is a slim G-buffer, not a visibility buffer.** Changed.
- The plan's visibility buffer stores a triangle id. That needs either 3 vertex invocations per triangle, or `primitive_index`, which Safari and the iPhone lack.
- Instead, the pass writes four targets:
  - RG32Uint ids (pick id, body);
  - an RG16F normal;
  - an R8 body tag for outlines;
  - an R8 ink target for edges, which keeps the larger coverage where they overlap.
- Ambient occlusion (RG16F, with its depth) and the shadow mask (R8) are at half resolution.
- That is 18 B per pixel at full resolution (about 95 MB at 2880×1800, 26 MB at the iPhone's DPR 2), against the plan's 12.5. The extra memory buys about 1.85× on geometry.

**3. Geometry: one cull pass, exact index ranges, one indexed indirect draw.** Changed from 16-triangle clusters with vertex pulling.
- One compute pass culls meshlet instances, for the view, the shadow and the edges.
- Each visible meshlet gets exactly its triangles' indices.
- One indexed indirect draw per kind (closed, open) then reuses vertices.

The results:
- the 10M STL's geometry pass takes 1.36 ms where the clusters took 2.52 ms, level with Metal's mesh shaders;
- the iPhone holds 60 fps on it;
- exact ranges keep a million small parts to 144 MB of indices.

**4. Picking is asynchronous, through a ring of readback buffers.** Confirmed. The answer arrives before the next frame everywhere, so `nav::PixelPicker` needs Pending only within a frame.

**5. Threads: the engine on the main thread, import in Web Workers.** Confirmed, with:
- **one compiled module** posted to a pool of workers kept warm;
- **one worker per performance core** on desktops, **two on the iPhone**;
- **a memory cap per worker (512 MB)**, so a runaway slice costs a worker and not the page.

The front end (tokenizer, structure, packer) uses threads where the page is cross-origin isolated, which on iPhone means serving from loopback (see 7).

**6. No native import accelerator.** WebAssembly import runs at 1.3–1.4× native per slice, far below the 3× that would have called for one. The desktop launcher may still use the native helper it already has, since it is fastest and costs nothing more.

**7. iPhone: the thin WKWebView shell, served over loopback HTTP.** Confirmed. Plan B (a native Metal iOS app) is not needed.
- WebKit honours cross-origin isolation only on HTTP responses, so the shell runs a small server on 127.0.0.1. It never leaves the phone and needs no permission. That is what gives the page threads.
- WKWebView renders at 60 Hz.
- iOS gives no memory warning before stopping a page, so the page keeps its own budget.

**8. The Mac: keep the Metal app for now; revisit after P3.** The plan's rule was to consider moving the Mac onto the web renderer if native Dawn came within 1.15× of Metal. On the measured frames it does: geometry 1.36 ms against 1.38 ms, and the whole Illustration frame 5.05 ms against the Metal app's 6.75 ms at 120 Hz. But:
- the spike lacks much of the Metal app: memoryless 4× MSAA, see-through, sections and caps, ghosts, and the measured golden images;
- you asked to keep the native Mac app for the best use of the hardware.

So the Metal app stays. The question returns when P3's renderer has those features and its own goldens.

**Answered by P3 (ADR 0014): the Mac keeps the Metal app.** With every feature and Metal's own images, WebGPU's one sample a frame is faster than Metal, at 0.43–0.90× of its time. At equal antialiasing it costs 1.19–2.4× on three of four scenes.

## Architecture

| Platform | Viewer | Import | AI integration |
|---|---|---|---|
| Mac | Native Metal app (as now) | Native helper processes | MCP launcher (S1), `.mcpb` |
| Windows, Linux, Mac browsers | Web viewer in the browser, served by the launcher on 127.0.0.1 | OCCT in Web Workers, or the launcher's native helper where one exists | The same launcher is the MCP server |
| iPhone | WKWebView shell over loopback HTTP | OCCT in two Web Workers | none in Phase 0 |

**Shared by all:** the cache format, the C++ engine core (compiled natively and to WebAssembly), the STEP front end, the packer, and the styles as data.

## Roadmap

**P1, platform split.** Turn W1's seams into the build's structure:
- portable engine layers, with Metal and WebGPU backends;
- a buffer-backed `scene::Segment` beside the mapped one;
- a `parseHeader` variant for partial files.

**P2, event-driven import, and Tier 0.** Done 2026-11-07, except Tier 0; results in `Docs/measurements/p2/p2-import.md`.
- Split `SliceImporter` into an event-driven core (`nextJob`, `onReply`, `onCrash`) with two drivers: native processes and Web Workers.
- The failure ladder stays.
- Fix the threaded packer's atomics, to meet W1's 2× gate.
- Warm worker pools and per-worker caps, from W2.

**P3, the WebGPU renderer.** Done 2026-11-07; see ADR 0014 and `Docs/measurements/p3/p3-renderer.md`. Moved on:
- the coarser shadow, which reduced detail makes unnecessary for now;
- progressive settled accumulation, to P4's frame loop.

The plan was, from the spike's indexed path:
- 16-bit indices;
- a shadow from a coarser level;
- reduced detail in motion for heavy scenes;
- accumulation of jittered frames once settled, for antialiasing;
- sections, see-through and ghosts;
- golden images against Metal's;
- a measured choice between edge setup in compute and per vertex.

**P4, the web runtime and interface.** Done 2026-11-07 on desktop browsers, and on the iPhone on 2026-11-08, where the memory gate passes; see ADR 0015 and `Docs/measurements/p4/p4-runtime.md`. Moved on: import memory and refinement in the browser (P7).
- One page that never navigates.
- Cache files streamed from fetch or OPFS straight into GPU buffers, with no WebAssembly or JavaScript copy. That is the largest saving on the iPhone, about 300 MB on the STL.
- A frame loop that submits nothing when idle, and adds one jittered sample per frame while the view is still.
- The framework-free interface.
- From P3:
  - shell classification through an asynchronous readback;
  - the overlay's text drawn with Canvas2D;
  - DPR 2 on phones.
- From P2: the import supervisor in a worker, and the warm pool started on a small slice.

**P5, launcher, MCP and packaging.** Done 2026-11-07; see ADR 0016. Extend S1's launcher:
- serve the web viewer;
- bridge the page and the AI app over a token-guarded WebSocket;
- package as `.mcpb`, and for `npx` and `~/.codex/config.toml`.

**P6, the iOS shell.** Built 2026-11-07, and run on the phone on 2026-11-08: the viewer, documents opened from outside, recovery and imports all work there; see `Docs/measurements/p7/p7-hardening.md`.
- Files and share-sheet input, served with Range.
- Recovery when the page's process is stopped.
- The loopback server's first-byte delay on large files (100–140 ms).

**P7, hardening.** Begun 2026-11-07 (`Docs/measurements/p7/p7-hardening.md`):
- done: the 1M rack's frame (8.3 to 5.3 ms, below the spike's 5.6 ms), fuzzing the streamed cache reader, GPU timing that cannot break frames, progressive STEP imports and stepwise antialiasing in the page, the faults the phone showed, and the browser's STL import heap halved (2.6 to 1.3 GB for 10M triangles);
- refinement in the browser, as on the Mac, from a STEP source the library keeps;
- to do: a Windows and a Linux run of the census and spikes, and on the iPhone: an STL import near the phone's new limit, refinement's memory, and the new cull's frame times (Safari on the Mac runs it without errors).

## Learned along the way

- **Paced GPU times measure the clock governor, not the renderer.** Compare unpaced, or at equal pacing; on the iPhone, render a burst of frames per refresh.
- **On the iPhone, runs that follow others in one web process came out 2–3× slower.** Earlier pages are probably kept alive by WebKit's back-forward cache. The viewer is one page; test runs need fresh launches.
- **Mac benchmarks need an idle machine.** Another heavy build on the same Mac turned a 1.35× WebAssembly ratio into an apparent 2–4.5×.
- **The test fixtures can move.** The hard file was archived to S3 during Phase 0. The perf gate takes `GEOMETRY_FIXTURES` to point at a copy.
