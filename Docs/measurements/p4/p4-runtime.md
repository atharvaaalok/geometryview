# P4: the web runtime and interface

Measured on 2026-11-07 (evening, Pacific Time) on the M4 Max:
- Chrome 157 and Safari 26;
- headless Chrome for screenshots.

The raw reports are in `runtime/`. The design is in ADR 0015.

**The iPhone** was measured the next night, 2026-11-08 at about 2:20–2:50 AM Pacific Time, once
it could be reached again: see "The iPhone" below. **The memory gate passes.**

## What P4 built

- **The engine in the page** (`Engine/browser/web_engine.cpp`, target `geometry-web`, 667 KB of
  WebAssembly). It is the portable core and the WebGPU renderer, with a host made of the page's
  frame loop, timers and callbacks, all on the main thread.
- **Cache files stream from fetch, a File or the library straight into GPU buffers.**
  - `scene::CacheStream` reads a file as its bytes arrive. Its segments go to a `SegmentSink`: the
    renderer, which writes them into a geometry arena.
  - Memory keeps only the sections the CPU reads: header, tree, definitions, meshlets and edge
    chunks. These are resident segments, validated by `format::parseSegmentParts`.
  - The page hands bytes over through one reused 1 MB staging buffer.
- **The geometry arena** places each segment once. The renderer used to upload every segment
  again whenever the scene changed. The arena grows by copying on the GPU, and a stream announces
  the file's size first, so its room is made once.
- **The frame loop submits nothing while the view is still**, after one frame drawn again with
  all four samples.
  - On a phone, that frame is skipped when four samples would take over 64 ms.
  - Heavy scenes leave out edges and the shadow while moving. Phones use a lower budget (3M
    triangles) than the Mac's 20M (`Core::setDetailBudget`).
- **Shell classification is asynchronous.** `Backend::requestOccurrenceIds` hands ids to a
  callback: at once on the Mac, a frame or more later in the browser. The core runs its two rounds
  from those callbacks, and drops views of a scene that has since changed.
- **The overlay's text is drawn with Canvas2D**, in IBM Plex Mono SemiBold, at the Mac atlas's
  cell positions.
- **Snapshots are read back asynchronously** (`geo_web_snapshot`): for checks now, and for the AI
  tools in P5.
- **The interface** (`Web/viewer/`, framework-free) mirrors the Mac app's chrome:
  - the masthead, the structure tree with search and row buttons, and the library;
  - the selection, look and section cards, and the toolbar;
  - the hold hint, the hover chip, the drop zone and the shortcuts sheet;
  - the Mac's palette, metrics and fonts. Only the tree rows in view are made into elements.

  Input follows the Mac viewport. On a phone, the tree is a sheet opened from the toolbar, and the
  view renders at DPR 2 at most.
- **STEP and STL import in the page.**
  - STEP uses the P2 supervisor in a worker of its own, with a warm OCCT pool that outlives each
    import.
  - STL uses the supervisor's new `mesh_import`, multithreaded where the page is cross-origin
    isolated, in a worker that is ended afterwards.
  - Finished cache files are kept in the origin-private file system, with a note of their source,
    so an unchanged file opens again from there.
- **The iOS shell bundles the viewer and its importers** (`loopback:viewer:<query>`).

## Opening and drawing

Full-window pages at 3456×1992 (Safari 3456×1986). Each value is from one run. "Load" runs from
asking to open a fixture to the scene being ready: fetching it from the local server, streaming
and syncing.

| | Chrome, pack | Chrome, 10M STL | Safari, pack | Safari, 10M STL |
|---|---|---|---|---|
| Page to engine ready | 121 ms | 108 ms | 86 ms | 47 ms |
| Load | 136 ms | 325 ms | 113 ms | 220 ms |
| Frames submitted in 3 s still | 0 | 0 | 0 | 0 |
| Frames over a 2 s drag | 118 | 118 | 118 | 118 |
| Four-sample frames after the drag | 1 | 1 | 1 | 1 |
| Hover pick under the pointer | hit | hit | hit | hit |
| WebAssembly heap | 16 MB | 16 MB | 16 MB | 16 MB |
| GPU buffers | 5.6 MB | 268.8 MB | 5.6 MB | 268.8 MB |
| Render targets | 205 MB | 205 MB | 205 MB | 205 MB |

- **The heap stays at 16 MB with the 10M STL open**, where P3's bench page held 363 MB. The file
  is never in WebAssembly or JavaScript memory whole, only a megabyte at a time.
- **GPU buffers are as P3 measured.** Reading a file whole natively and streaming it
  (`geometry-webgpu bench --stream`) both give 268.8 MB.
- **A first version of the stream placed the first segment before the host reserved room**, which
  added a second copy of the file's size: 419.6 MB. The stream now tells its sink the size first.

## Importing

A cold pool means the page's first import: each run is a fresh page. Chrome and Safari use 12
OCCT workers. Times run from the file in hand to the scene ready, and include storing the cache
file in the library.

| | Chrome | Safari | P2's page (cold) | Native |
|---|---|---|---|---|
| PLE120, 25 MB STEP | 0.78 s | | 0.58–0.61 s (Chrome) | 0.18 s |
| Hard file, 52.7 MB STEP | 3.17–3.21 s | 4.87 s | 2.82–2.99 s Chrome, 4.64–4.76 s Safari | 1.73 s |
| 10M STL, 500 MB | 3.05 s | 4.39 s | | 1.29 s |
| Hard file, again, from the library | 19–21 ms | | | |

- **The same cache files result.** The hard file gives 340,646 triangles and 12,188 faces in both
  browsers, as P2 found.
- **The STL imports in 3.05 s on threads, against 16.2 s on one.** P2's threaded packer is in the
  page now. The supervisor's heap peaks at 2.6–2.75 GB, though: the 500 MB file, its mesh as it
  is packed and the cache file at once. Ending its worker frees all of it. The supervisor may now
  grow to 4 GB.

## The iPhone

The iPhone 16 Pro, in the P6 shell over loopback HTTP, at DPR 2 (804×1748). The viewer was built
with P7's first round (progressive STEP imports, four samples one a frame). Reports are in
`runtime/iphone-*`.

**The memory gate passes.** The 10M STL open in the viewer, from Instruments' Activity Monitor
template:

| Process | P4 viewer | P3 bench page | Phase 0 spike |
|---|---|---|---|
| WebContent (page and WebAssembly) | 390–470 MB | 938 MB | 868 MB |
| GPU process | 160–225 MB | 264 MB | 226 MB |
| **Both at once** | **at most 630 MB** (the two peaks, summed: 695 MB) | 1,202 MB | 1,094 MB (gate: 900 MB) |

- Streaming the cache into GPU buffers removed the CPU copies, as planned: the WebAssembly heap is
  16 MB. The renderer holds 268.8 MB of buffers and 41.9 MB of targets.
- Instruments kept the phone for whole recordings this time (18–20 s each, sampled about once a
  second). The page was never stopped.

**The frame loop on the phone**, from the viewer's scripted check (`test=idle`):

| | Pack | 10M STL |
|---|---|---|
| From navigation to an engine ready to draw | 62 ms | 50 ms |
| Frames in 3 s still | 0 | 0 |
| Frames in a 2 s drag | 118 | 118 |
| Frames after the drag settles | 1 | 1 |
| Hover picks that found a face | 35 | 1 (one face) |

The STL holds 60 fps while dragged because the phone's detail budget (3M triangles) leaves out
edges and the shadow in motion.

**Imports on the phone**, with 2 workers, each shown as it imports:

| File | Time | Triangles shown | Memory at peak |
|---|---|---|---|
| Pack | 2.0–2.3 s | 331,520, all | |
| PLE120 | 3.4 s | 116,024, all | |
| Hard file (52.7 MB) | 19.9 s (gate 30 s; P2's import page 16.5–18.2 s) | 340,646, all | WebContent 570 MB, GPU process 135 MB |

The viewer's import peaks higher than P2's import page (464 MB in all). It also holds the viewer,
its engine, the file as fetched, and the cache as it goes to the library.

## The interface

Headless Chrome screenshots, at 1600×1000 and as an iPhone 16 Pro (402×874), showed:
- the masthead's counts;
- the tree with material swatches, selection and its card;
- the look and section cards;
- see-through, classified asynchronously, with the X cut's hatched caps;
- the phone layout.

The Mac app's look carries over closely.

## The Mac app

P4 changed the core (classification, the detail budget) and the scene and format layers.

| Check | Result |
|---|---|
| Unit tests | pass in debug, ASan and TSan |
| Unit tests as WebAssembly | 169 pass, 1 skipped |
| Dawn suite | passes, now with "a streamed file draws as one read whole", identical to the pixel |
| `--exercise` | 59.7 fps on the 60 Hz display, 0 3D frames from chrome-only changes; 54 allocations in its last frame, as before |
| `--chrome-check` | passes |
| `Tools/perf_gate.py` | the pack and the STL flagged at a load average of 9–10; `main` and P4 run alternately import them in the same time (84 ms and 84 ms; 1.292 s and 1.291 s) |

## Open

- **Import memory.** The STL import's 2.75 GB peak is far over a phone's budget, which now refuses
  STL files over about 1.2 GB. The importer should stream the file and write its cache in segments.
- **Refinement** (finer geometry for a close view) is refused in the browser. It needs OCCT for
  an open file.

Done in P7's first round: imports show as they go, and a still view's four samples are drawn one a
frame (`p7/p7-hardening.md`).
