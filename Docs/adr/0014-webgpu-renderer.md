# ADR 0014: The WebGPU renderer

Status: accepted (2026-11-07). P3 of ADR 0013. Measurements: `Docs/measurements/p3/p3-renderer.md`.

## Context

ADR 0013 gave P3 these goals:
- the web renderer draws what the Metal app draws;
- golden images match Metal's;
- it holds the Phase 0 frame budgets on the Mac's browsers and the iPhone.

It also asked P3 to settle the open choices: antialiasing, the shadow, reduced detail and edge
setup. P3 is also where decision 8 (whether the Mac should move onto the web renderer) is answered.

The renderer is `geo::webgpu::Renderer` (`Engine/webgpu/`): a `render::Backend` in portable C++
over `webgpu_cpp.h`. It builds natively on Dawn and to WebAssembly with `emdawnwebgpu`. It shares
the scene, styles, looks, view and overlay code with Metal, and reads the cache's GPU records
unchanged.

## Decisions

**1. Forward shading in a single-sample main pass, not the spike's G-buffer.**
- **The main pass writes five targets in Metal's order**:
  - colour (RGBA8);
  - ids (R32Uint);
  - a depth copy (R32F, the depth at the pixel's centre);
  - ink (RGBA8);
  - tags (RG8Uint);
  - plus D32 depth with reversed Z.
- **Occlusion and the composite read them as Metal's do.** The WGSL is a line-by-line port, and
  styles, states and the composite stay one design on both renderers.
- **The cost:** 31.25 bytes per pixel in all (154.5 MB at 2880×1800, 42 MB at the iPhone's DPR 2).
  That is more than the spike's 18, but:
  - the STL's whole frame is still 5.3 ms in Chrome, against the spike's 5.05 ms with far less;
  - the iPhone's memory problem is the CPU copies (decision 8), not the targets.

**2. Antialiasing: Metal's four sample positions, as jittered passes accumulated.**
- Each sample's main pass is drawn and inked by a resolve pass, then added into an RGBA16F target
  at a quarter weight. Occlusion and the composite run once, on sample 0, which is drawn last, as
  Metal reads sample 0.
- Edges and the overlay are evaluated by Metal at the pixel's centre for all four samples.
  `FrameUniforms.sampleOffset` moves their varyings back there, so lines keep Metal's width and
  softness.
- **Frames in motion take one sample**, faster than Metal's 4× MSAA on every scene measured
  (0.43–0.90× of its GPU time).
- **Settled frames take four**, which costs 1.19–2.4× of Metal's 4× MSAA.
- P3 draws all four in one frame. P4's frame loop adds one per frame while the view is still.

**3. Culling into ordered lists, drawn with 16-bit indices in chunks.**
- **One compute pass culls each meshlet instance** for seven lists: solids, ghosts and the
  see-through shell, each split into closed and open, plus the cast shadow.
- **It counts, scans and writes**, so every list is in instance order. Compaction by atomic append
  ordered meshlets as threads finished, and coincident faces z-fought differently each frame.
- **Each visible meshlet gets its exact index range.** An index is
  `(slot % 512) << 7 | vertex`, drawn per chunk of 512 slots with `baseVertex = chunkBase << 7`.
  That halves index memory against 32-bit indices.
- **Cone culling is skipped for lists whose back faces show**: an open section, and the shell's back
  layer. Meshlets wholly in a cut half are culled.
- **The CPU sizes each list** from the states and looks, so memory follows what can be drawn.

**4. Sections clip in the fragment shader.** Safari has no `clip-distances`. A discard costs
little, since meshlets wholly in the cut half are already culled.

**5. Edges are expanded per vertex**: six vertices per segment, read from the cache's segment
records. They cost about 1.6 ms of the STL's 5.3 ms on Dawn. The plan's alternative (a compute
setup pass) was not built. It is worth trying only if edges become the bottleneck.

**6. Pipelines are made on first use, and each stage gets only the override constants it reads.**
- A `PipelineKey` selects the pipeline. It holds the kind of pass (solid, ghost, a shell layer,
  shadow, edges, classification) and switches for the physical light, the section, closed parts,
  picking, ghost lines and peeling.
- Safari refuses a pipeline whose stage is given a constant it does not declare.
- The uniform slots are spaced by `sizeof(FrameUniforms)` rounded up to 256 bytes (416 bytes now),
  never by a fixed 256.

**7. Picking is a probe read back each frame.**
- The probe is a 64×64 window of ids and depths at the pointer, plus a 3×3 grid of samples for
  orbit pivots.
- It goes through a ring of three readback buffers.
- Callbacks hold a `weak_ptr` token, so a renderer destroyed mid-readback is safe.
- On the Mac's browsers and the iPhone, every probe came back. On the iPhone's pack, every pick hit.
- The blocking calls are native only, and the browser build leaves them out:
  - `readPixel`, from the newest probe;
  - `renderOccurrenceIds`, for shell classification.

  P4 gives classification an asynchronous path.

**8. The Mac keeps the Metal app.** This answers ADR 0013's decision 8.
- **The rule:** move the Mac onto the web renderer only if it came within 1.15× of Metal.
- **At equal antialiasing it does not.** Four jittered samples cost 1.19× Metal on the pack, 1.39×
  on the 10M STL and 2.4× on the 100k rack. Only the 1M-part rack is faster (0.83×).
- **One sample a frame is faster everywhere, but aliased in motion.** The Metal app is not.
- **Why:** memoryless 4× MSAA in tile memory has no WebGPU equivalent.

**9. Reduced detail in motion, as on the Mac.** `setReducedDetail` drops edges and the cast shadow.
- On the iPhone that takes the 10M STL from 30.6 ms to 12.8 ms a frame: from about 30 fps to 60.
- The coarser shadow level from the roadmap is deferred. Reduced detail already removes the shadow
  in motion, and a settled frame can afford it.

## Consequences

- **Metal's own shaders were fixed by the comparison:**
  - safe math where depth becomes positions, which cured perspective speckle;
  - no cone culling where a cut or the shell's back layer shows back faces, which restored missing
    caps;
  - the cut-half cull.
- **Every golden scene and state is tested against Metal** in `Tests/unit/webgpu_test.mm`, natively
  on Dawn. Real models are under 0.27% of pixels more than 8 levels off.
- **The iPhone's memory gate (900 MB) still fails** with the 10M STL: 1.2 GB across the page and
  GPU processes. The renderer's share is 269 MB of buffers and 42 MB of targets. Most of the rest is
  the page's three CPU copies of the 151 MB cache file, which P4's streaming removes.
- **The 1M-part rack's frame is 8.3 ms, against the spike's 5.6 ms.** The ordered cull, states and
  looks are the likely cost, but it is not yet profiled (P7).
- **The overlay's text comes from the host** (`setOverlayAtlas`). The Mac draws it with Core Text.
  The web runtime will draw it with Canvas2D.


## Addendum: pictures for the gallery (2026-11-11, ADR 0017)

The renderer carries alpha through the sheet and the composite, as Metal does, and draws
offscreen pictures without waiting (`renderOffscreenAsync`), since the browser cannot wait. The
gallery's pictures are drawn by a module of their own, `geometryview-thumbs-web`, in a worker
with its own device.
