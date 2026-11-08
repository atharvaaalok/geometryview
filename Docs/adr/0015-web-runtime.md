# ADR 0015: The web runtime

Status: accepted (2026-11-07), with the iPhone measurements still to come. P4 of ADR 0013.
Measurements: `Docs/measurements/p4/p4-runtime.md`.

## Context

P3 left a renderer that draws the Mac app's images in the browser. P4 makes it a viewer:
- one page that never navigates;
- cache files streamed to the GPU without whole copies in memory, since the iPhone's memory gate
  failed on them;
- a frame loop that submits nothing when idle;
- the interface;
- imports in the page.

It also took on P3's leftovers: asynchronous shell classification, the overlay's text drawn by
the page, and DPR 2 on phones.

## Decisions

**1. The engine runs on the page's main thread, as one WebAssembly module with a small host.**
- `Engine/browser/web_engine.cpp` gives the core the `Host` it asks for:
  - the browser's animation frames as the frame clock;
  - `setTimeout` for refinement;
  - the page's callbacks (`Module.geoHost`) for events and imports.
- The page calls `geo_web_*` functions.
- Nothing is proxied to a worker: the core's work per frame is small, and WebGPU's device belongs
  to the thread that made it. Errors thrown by the page's callbacks are caught at the edge, so
  they never unwind through the engine.

**2. Cache files stream straight to the GPU; memory keeps only what the CPU reads.**
- The CPU reads a segment's header, tree, definitions, meshlets and edge chunks; only the GPU reads
  its vertices, triangles, faces and edge segments.
- So `CacheStream` routes each arriving byte:
  - the whole segment to its placement in the renderer's geometry arena;
  - the CPU's sections also to a compact copy, which `parseSegmentParts` validates as before.
- Untrusted bytes reach the GPU before validation, but nothing draws from a segment until it is
  validated and added to the scene.
- The page hands bytes over through one reused 1 MB staging buffer. The file is never whole in
  WebAssembly or JavaScript memory. The 10M STL opens with a 16 MB heap.

**3. Segments are placed once, in an arena.**
- The renderer's geometry is one storage buffer, as before: binding limits leave no room for one
  per segment.
- Each segment is placed in it once, instead of every segment being uploaded again whenever the
  scene changed. That was quadratic in segments and impossible for resident ones.
- The buffer grows by copying on the GPU, and freed ranges are reused.
- A stream announces its file's size before placing anything, so a file's room is made once.

**4. Idle means no submits; a still view is drawn once more with four samples.**
- The host draws only when the core has a frame.
- About 0.35 s after the last one, it draws that frame again with Metal's four sample positions,
  then stops the loop.
- On phones it skips that frame when four samples of the last frame would take over 64 ms. It is
  drawn in one frame, and a touch would wait for it.
- Phones also leave out edges and the shadow while moving at a lower budget (3M triangles) than
  the Mac (20M).

**5. Shell classification arrives by callback.**
- `Backend::requestOccurrenceIds` answers at once where the renderer can wait for the GPU (Metal,
  Dawn), and when the readback maps in the browser.
- The core keeps a classification's state between views, and a newer classification or closing
  everything makes the old one stale.
- The Mac's behaviour is unchanged.

**6. The interface is plain modules mirroring the Mac app's chrome.**
- No framework.
- Components follow `App/Sources/Design` and `App/Sources/Chrome`: the same palette, line weight,
  radii, offset shadows, IBM Plex Mono and wording.
- The tree builds rows by jumping over collapsed subtrees, as the Mac's does, and makes elements
  only for the rows in view.
- On a phone, the tree is a sheet opened from the toolbar.

**7. Imports run in workers, and their results go to the library.**
- **STEP:** the P2 supervisor runs in a long-lived worker with a warm OCCT pool. Each import gets a
  fresh supervisor instance, so its memory goes with it.
- **STL:** it needs no OCCT. Its import runs multithreaded where the page is cross-origin isolated,
  in a worker that is ended afterwards: a threaded module's memory lives as long as its threads.
- **The library:** finished cache files go to the origin-private file system with a note of their
  source. The same unchanged file opens from there next time, in about 20 ms on the hard file.

## Consequences

- **On desktop browsers the runtime meets every P4 aim:**
  - 0 submits when idle;
  - every drag frame drawn;
  - the 10M STL open with a 16 MB heap and the same 269 MB of GPU buffers as P3;
  - imports within about 1.1 times P2's cold times, including storing the result.
- **The iPhone's memory gate is not yet measured.** The page process held 938 MB with the STL in
  P3, of which about 450 MB were the bench page's copies of the file. P4 removes them, so the gate
  should pass, but that is not yet measured. *Measured on 2026-11-08: it passes, at most 630 MB
  (`p4/p4-runtime.md`).*
- **Imports of large STL files need far less memory before they can run on a phone.** The 10M
  STL's import peaks at 2.75 GB. Streaming the file through the importer and writing the cache in
  segments is P7's work, with progressive display of imports and refinement in the browser.
  *Done in P7: a binary STL is read in pieces and the 10M import's heap is 1.27–1.37 GB; imports
  show as they go; STEP files refine in the browser from a source kept in the library
  (`p7/p7-hardening.md`).*
- **The Mac app keeps its behaviour**, with its tests, `--exercise` and `--chrome-check` as before.
