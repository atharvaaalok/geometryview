# R: the WebGPU renderer spike

Measured 2026-11-07:
- **Mac:** M4 Max (40-core GPU), on Dawn natively, Chrome 157 and Safari 26.
- **iPhone:** iPhone 16 Pro, inside the shell from S3.

The spike is one C++ renderer against `webgpu.h` in `Tools/spikes/webgpu`. It builds natively on Dawn (preset `dawn`) and as WebAssembly with emdawnwebgpu (preset `wasm-webgpu`). Pages are in `Web/spike`, and raw reports in `renderer/`.

The fixtures are cache files made by `gcli import`:
- the 10M-triangle STL;
- the 13S4P pack (331k triangles, 837 parts);
- racks of 100k and 1M twelve-triangle boxes, from `Tools/make_big_assembly.py`.

Every run is an orthographic orbit at three zooms (1, 2 and 4), 6 s each.

## How to read GPU times

**Paced runs measure the clock, not the renderer.** Apple GPUs lower their clock until the work just fits the display's frame, so a paced frame's GPU time is mostly set by the governor. The same 10M-triangle frame takes:
- 2.7 ms on Dawn when frames run back to back;
- 6.1 ms paced at 120 Hz;
- 8.6 ms paced at 60 Hz.

Comparisons here are therefore made in one of two ways:
- **unpaced**, at full clock;
- **at equal pacing.**

On the iPhone, unpaced means burst mode: three frames per display refresh, so the GPU never idles.

**The browser adds nothing to GPU time.** Unpaced, Chrome took 2.72 ms where native Dawn took 2.68 ms for the first renderer, and 1.82 ms against 1.83 ms for the indexed one. Encoding and submitting a frame costs 0.04–0.08 ms of CPU in Chrome, and 0.2–0.3 ms on the iPhone.

**Apple GPUs overlap consecutive passes**, so per-pass times are approximate and only frame totals are exact. Dawn rounds timestamps to 65.5 µs unless its `timestamp_quantization` toggle is off. Chrome rounds them to 100 µs.

## The geometry pass: pulled against indexed

Two ways of drawing were compared. Both cull meshlet instances in one compute pass, then draw with a single indirect call.

| | Pulled (the plan's design) | Indexed (adopted) |
|---|---|---|
| Draw | 16-triangle clusters, no index buffer, 3 vertex invocations per triangle | the cull pass writes each visible meshlet's indices, so vertices are reused |
| Per pixel | a triangle id, which the composite turns back into a normal | the normal (RG16F), ids (RG32Uint) and a body tag (R8Uint) |
| Why the per-pixel choice | | without `primitive_index`, which Safari and the iPhone lack, a per-triangle id cannot ride an indexed draw |

| 10M STL, 2880×1800, unpaced | Pulled | Indexed | Metal mesh shaders (ADR 0001) |
|---|---|---|---|
| Geometry pass | 2.52 ms | **1.36 ms** | 1.38 ms at 3456×2234 with 4× MSAA |
| Frame, surfaces and a simple composite | 2.67 ms | **1.83 ms** | |
| iPhone 16 Pro, DPR 2, paced at 60 Hz | 20 ms (45 fps) | **13.2 ms (60 fps)** | |

The two modes draw the same image: 1 of 5.2 million pixels differs. Culling 86k meshlets takes 12 µs.

**Decision:** the indexed mode. Its geometry pass is level with the Metal app's mesh shaders, so a WebGPU Mac would lose little on raw geometry. Decision 8 still keeps the Metal app, for the features it has that WebGPU lacks (memoryless MSAA, tile memory).

## The full Illustration frame

The indexed mode also draws:
- feature edges, as screen-space quads in an ink target;
- the cast shadow, by running the cull and vertex stages again with a pipeline override that flattens geometry onto the ground, at half resolution;
- ambient occlusion from depth, at half resolution;
- a composite with outline search, fold lines, blurred occlusion and the shadow on the sheet.

These follow `Engine/shaders/*.metal`. The Metal app draws the 10M STL's 1.1M crease edges with the same speckle.

| M4 Max, 2880×1800, all features | WebGPU unpaced | WebGPU at 120 Hz | Metal app at 120 Hz (ADR 0009) |
|---|---|---|---|
| 10M STL | 5.05 ms (p99 5.8) | 5.26 ms (p99 8.1) | 6.75 ms |
| Rack of 1M parts | 5.62 ms | 5.63 ms | 11.8 ms |
| Rack of 100k parts | 3.27 ms | | 4.7 ms |
| 13S4P pack | 1.5 ms | | 4.2 ms |

Metal's figures are means; WebGPU's are p50. On the STL, edges cost about 1.4 ms, the shadow about 1.6 ms and occlusion under 0.1 ms.

**The plan's gate** was Mac Chrome p50 ≤10.1 ms on this frame. It passes at about half that.

## iPhone 16 Pro

| | Capacity (burst) | Paced at 60 Hz, fresh launch |
|---|---|---|
| Pack, all features, 1206×2622 (DPR 3) | 6.1–6.4 ms | 8.4–8.8 ms p50, about 15 ms p99 |
| Pack, all features, 804×1748 (DPR 2) | | 8.4–8.6 ms p50, about 15 ms p99 |
| 10M STL, surfaces only, DPR 2 | 9.7–14.8 ms | 13.2–13.9 ms, 60 fps |
| 10M STL, all features, DPR 2 | 34 ms | 34 ms (29 fps) |

On the STL the shadow costs 9 ms and the 1.1M edges about 6 ms.

**The plan's gate** was the pack at DPR 2 within p99 ≤16.7 ms. It passes, and at the phone's full resolution too. WKWebView renders at 60 Hz (S3).

**Run order matters on the phone.** Runs that followed others in the same web process came out 2–3× slower than fresh ones (24.5 ms against 8.7 ms). That is likely the earlier pages kept alive by WebKit's back-forward cache with their GPU resources. The viewer is one page that never navigates, so this harness launches the app fresh for each run.

## Hover picking

Each frame copies the ids under a sweeping cursor to a ring of three buffers and maps them asynchronously.

| | Answer arrives | Time after submit (p50 / p99) |
|---|---|---|
| Chrome | before the next frame | 4.8–5.8 / 5.3–6.7 ms |
| Safari | before the next frame | 6.7–7.8 / 9.8–11.0 ms |

**The gate** was p99 ≤2 vsyncs. It passes with a vsync to spare, and that holds at 120 Hz as well.

## Memory

GPU memory the renderer holds, by its own count:

| | Buffers | Targets |
|---|---|---|
| 10M STL | 381 MB: 151 MB of cache data, plus 115 MB of indices each for the camera and the shadow | 95 MB at 2880×1800, 26 MB at DPR 2 |
| Rack of 1M parts | 362 MB | |
| 13S4P pack | 9 MB | |

Indices are sized to the triangles drawn: each visible meshlet reserves exactly its own. Fixed per-meshlet slots would have needed 1.5 GB for the 1M-part rack.

### The iPhone soak

The 10M-triangle STL orbited for 10 minutes at DPR 2 with every feature on. That is the worst case, drawn with the fixed index slots of the time (398 MB of buffers). Instruments' Activity Monitor template recorded every process's memory (`renderer/iphone-soak600s-memory.json`).

| Process | Start | Peak | End |
|---|---|---|---|
| WebKit web content (the page) | 829 MB | 868 MB | 866 MB |
| WebKit GPU | 142 MB | 226 MB | 18 MB |
| The shell | 14 MB | 14 MB | 12 MB |

**The page was never stopped and its memory never grew.** Frames held 25–40 fps as the phone heated, and the thermal state ended "serious". Every hover answer came back before the next frame.

**Against the plan's gate** of 900 MB for page and GPU together, about 1.09 GB at peak is over by a fifth. The page's GPU buffers count toward the page, and they were about 400 MB. Most of the rest is two whole extra copies of the 151 MB cache file: one in WebAssembly memory and one in JavaScript. Both are avoidable.

| Fix for P4 | Saves (approx.) |
|---|---|
| Stream the cache straight from the fetch into GPU buffers: no WebAssembly copy, and the JavaScript one dropped as it uploads | 300 MB |
| Exact index ranges (done since the soak) | 17 MB |
| 16-bit indices | 115 MB |
| No shadow indices while heavy scenes move | 115 MB |

Together these bring the STL to roughly 450–550 MB, well inside the gate.

## What this decides for P3

**Adopt the indexed visibility pass:**
- exact index ranges;
- per-pixel normal, ids and a byte tag;
- one compute cull per frame for surfaces, shadow and edges.

**Heavy scenes in motion drop edges and shadow,** as the Metal app does. On the iPhone that keeps the 10M STL at 60 fps (14 ms); the full style is drawn once the view settles.

**Hover is asynchronous and arrives within a frame,** so `nav::PixelPicker` can use it without Pending states that last.

## Still open for P3

- **16-bit indices**, by batching slots under a base vertex, would halve index memory. So would a shadow drawn from a coarser level, which would also cut its 9 ms on the phone.
- **Settled accumulation** of jittered frames for antialiasing; the visibility pass is single-sampled.
- **Comparing edge setup in a compute pass** against recomputing it per vertex.
- **Uploading cache segments from JavaScript** straight into GPU buffers, without the WebAssembly heap copy the spike makes. This is the largest memory saving on the phone.
- **A Windows or Linux run.**
