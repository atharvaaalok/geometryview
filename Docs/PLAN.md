# GeometryView: an extreme-performance native CAD viewer for macOS

## Context

The repository began with only the requirements brief. We are building a standalone viewer for STEP, STL and similar files, from scratch. Priorities in order: speed, memory, leanness. It must also be beautiful and highly maintainable, with small atomic commits. Users open their own files or connect whole directories. The author's earlier viewers and prototypes (step_viewer among them) and Colorfigures are references for behaviour and look, and no code is copied.

**User decisions:** macOS only with native Metal. OCCT in a separate helper with our own fast paths. Wheel zooms toward the cursor by default (about-target stays in Settings). Clicking a file opens it alone, and Cmd-click adds it to the same viewport.

**Evidence labels:** *measured* = run on this M4 Max during planning. *target* = a budget to hit, not yet achieved. Planning prototypes were throwaway, so M2 and M0 must reproduce the key results natively.

## Findings that shaped the design

| Finding (measured) | Consequence |
|---|---|
| Across 300 CADBench files, CPU time was Transfer 64%, meshing 24%, ReadFile 12%. ShapeFix is about 90% of Transfer | A faster parser alone does not fix import. Attack Transfer |
| Slicing STEP text into about 100 kB face chunks, each read by unmodified OCCT in its own process: PLE120 1.96 → 0.22 s, pack 2.62 → 0.38 s, 52.7 MB file 19.2 → 1.5 s | **The central import bet.** 320/320 files matched on face count, area within 0.1%. One file was 8% off on signed volume (probably dropped `ORIENTED_CLOSED_SHELL` flip) |
| The pack's 837 solids reduce to 71 byte-identical ones; with slicing it drops to 0.065 s | Hash-based instancing, because XCAF reports 533/533 unique |
| A NEON tokenizer handled 792 MB in 0.05 s at 12 threads, about 350× OCCT ReadFile (ids, keywords and references only, warm cache) | The tree appears in milliseconds |
| OCCT peak: 3.62 GB on 52.7 MB; 16.7 GB and 134.6 s on the 792 MB file | OCCT can never live in the viewer process |
| Angle 0.06 → 0.35 rad cuts triangles 2.6–5×. On demo_drive, dropping `MinSize` costs 5× (91k vs 468k tris) | Coarse default, refine by screen-space error |
| Mesh shaders: 9.2 Gtri/s at 98 threads per threadgroup vs 7.8 classic, but 2.5–2.7× slower if serial. Mesh loses below about 60% meshlet fill | Use only the wide form, on synthetic geometry so far, gated by a spike |
| Binding a file-mapped Metal buffer wires the whole mapping (174–185 ms for 500 MB cold), and read speed equals private memory | Zero-copy works, but there is no demand paging. Chunk the data |
| Hidden-surface removal (HSR): late discard or depth write costs 8–10×. `clip_distance` costs nothing until triangles are cut | No discard in the opaque path |
| Render targets total about 134 MB at 3456×2234 with 4× MSAA, vs a 655–910 MB estimate for step_viewer scaled to this size | Memoryless tile attachments |
| Reading linear ID and depth costs 0.6 ns per pixel on the CPU | Picking and pivots need no GPU round trip |
| Colorfigures see-through is one flat alpha per layer, not order-independent transparency (OIT). Imageblock OIT costs about 3.9× simple alpha | No OIT. Layered alpha reproduces the look exactly |

**Caveats:** the CADBench sample contains no assemblies or class-A surfacing, so analytic-surface shares are upper bounds. GPU numbers come from uniform synthetic spheres in burst runs. OCCT numbers come through Python bindings with unknown build flags.

## Language and stack

| Layer | Choice | Why |
|---|---|---|
| Engine, parsers, helper | **C++23**, no exceptions or RTTI (OCCT adapter excepted) | Shares headers and inline packing code with Metal Shading Language (MSL). Same language as OCCT. NEON intrinsics are usable directly |
| Metal backend | Objective-C++ with ARC, confined to `gpu/` | It is the SDK itself; nothing extra to vendor |
| UI | **Swift 6 + custom-drawn AppKit** | Native text, input methods, accessibility and panels, with compile-time main-thread safety |
| Boundary | Plain C API (`engine.h`, about 30 functions) | Avoids Swift/C++ interop spreading through targets |
| Tooling | Python for dev scripts only | 240 MB baseline vs 17 MB for the same OCCT natively |

Rust is the strongest alternative, but it loses: MSL can't share Rust code, OCCT keeps C++ in the project anyway, the UI would still be Swift (three languages), and the helper already isolates bad input.

**Platform floor:** macOS 26.0, Metal 4 API, Apple7 GPU family. This Mac runs 26.6.2 with the 27.2 SDK, so APIs marked `macos(27.0)` are off limits. Native controls render as Liquid Glass with no opt-out, so the sticker chrome is custom-drawn.

## Architecture

```
Main thread        Swift AppKit: chrome, events. Never blocks.
  │ SPSC command ring + atomic latest-cursor
Engine thread      Single writer of all view state; also renders. Hosts display link.
  │ immutable UI snapshots back to Swift
Import supervisor  Own process, no OCCT: tokenize, tree, hash, slice, pack
  └ OCCT workers   N single-threaded processes; exit when the queue drains
Cache blob         Append-only, written by the helper, mapped read-only by the viewer.
                   Doubles as the transport, so a miss and a hit share one code path.
```

- **Multi-file scene:** a `Scene` is N mapped blobs plus an occurrence list, and each file is a root in the assembly tree. Cmd-click is then a small feature. Background documents keep view state and release GPU memory.
- **Numbering:** occurrences in depth-first order, so a tree node is an interval and bulk hide costs O(intervals).
- **Face state, two levels:** one word per occurrence, with a per-face page only for occurrences with face overrides. All others share a zero page. The object stage reads the page base once per meshlet. step_viewer's flat scheme would need 100 MB at 10,000 × 1,000 faces.
- **Pick ID, 31 bits plus cap bit:** `occurrenceFaceBase + localFace`, with the occurrence recovered by binary search on the CPU. IDs may be flattened; state must not be. Fallback to RG32Uint if a scene overflows.
- **Undo:** delta log per transaction, bounded by bytes. Covers hide, ghost, ghost style and isolate.
- **Selection:** sorted tagged 64-bit targets (face, body, part, node), the single source for tree and viewport.

## Import pipeline

1. **Tokenizer** (supervisor process): parallel `pread` in 8 MB blocks. Pass A counts quotes per chunk so each chunk's string state is exact. Pass B finds `;` outside strings with a NEON prefix-xor mask. A `/*` in DATA forces one sequential pass. Output is a flat SoA table. Decode ids, types and references eagerly, floats and strings lazily.
2. **Tree, names, colours, units** from our own parser, replacing XCAF. Handle NAUO/CDSR, `MAPPED_ITEM`, reversed representation order, `BREP_WITH_VOIDS`, `FACETED_BREP`, `TESSELLATED_*`, per-representation units and external references. `&SCOPE` or unknown structure falls back to whole-file OCCT.
3. **Dedup:** canonical hash of each solid's closure after renumbering.
4. **Slice:** face closures in about 100 kB chunks plus a synthetic `OPEN_SHELL`, carrying shell orientation.
5. **OCCT workers:** `ReadStream` → `TransferRoots` with `read.step.product.mode=0` → `BRepMesh_IncrementalMesh` with `InParallel=false` → per-face extraction through `STEPConstruct::FindShape`.
6. **Packer:** meshlets and analytic face records per definition, then `DEF_READY` to the viewer. Largest on-screen occurrence counts go first, so geometry appears progressively.

**Failure ladder:** bisect chunk → solid → definition → whole file → marked placeholder in the UI. Per-solid signed-volume check.
**Limits:** kill a worker at 2 GB, recycle after 512 MB, cap the pool at min(25% RAM, 16 GB). IGES runs one file per process.
**Cancellation:** close, then SIGTERM, then SIGKILL.
**OCCT build:** 8.0.1 from source, minimal toolkits, no Draw, Visualization, FreeType or FreeImage. One flag selects static or dylib. Start static for speed, and resolve LGPL obligations before any distribution.

**Tessellation default:** linear = diag × 2e-4, angular 0.35 rad, `MinSize` = diag × 1.5e-3. Refine a definition when projected chord error exceeds 0.5 px.

**Custom tessellator:** Tier 0 only (planar faces with line edges, `FACETED_BREP`, `TESSELLATED_*`), arriving in M8. It covers 73.5% of sample faces and the 792 MB file class. Do not write NURBS, torus or sphere trimming.

**STL:** binary if size = 84 + 50n, even with a `solid` prefix. ASCII is chunked by line, skips unknown lines such as `color`, and tolerates `\r` and missing `endsolid`. Weld with a partitioned hash. Pseudo-faces come from dihedral region growing plus small-region merging, so hover and hide work on meshes.

### Cache format

16 KB-aligned sections, little-endian, versioned, in chunks of 256 MB–1 GB because binding a buffer wires all of it. Request residency on a background thread.

| Section | Record | Notes |
|---|---|---|
| Vertices | 12 B: `u16 xyz, u16 localFace, i16 oct[2]` | `localFace` is **relative to a per-meshlet `faceBase`**, so definitions can exceed 64k faces (the 792 MB file has about 306k). Dense definitions add a 4 B high-bits stream for a 24-bit grid (ADR 0004) |
| Triangles | 4 B, 8-bit meshlet-local | |
| Meshlets | 32 B: bounds, cone, offsets, `faceBase` | 124 vertices and 124 triangles (changed from 64 and 124, see ADR 0001) |
| Occurrences | 64 B: 3×4 transform, definition, face base, flags | |
| Edges | Polylines with class crease/smooth/seam/boundary | Paged in only for line views |
| Faces, analytic | 32 B plus f64 parameters | Keeps later measurement possible |
| Tree, strings | Records, UTF-8 blob | Cold after tree build |

**Size target:** about 23–25 B/triangle, vs 36.6 in step_viewer. STL measured 12–14 B/triangle without edges or analytic records. The larger win is dedup plus coarser tessellation (pack about 31 MB → 1.3 MB, estimate).
**Keying:** (path, inode, size, mtime) as a fast path, then XXH3-128 of content. Tessellation settings, format version and importer stamp all invalidate. Persist only when the pipeline exceeds about 100 ms. LRU cap min(5% free disk, 20 GB).
**Security:** the blob reader treats input as untrusted, since a compromised helper could write it.

## Renderer

**One main pass and one composite pass cover every mode.**

| Pass | Work |
|---|---|
| Shadow (styles with shadow) | Flattened geometry into R8, blurred if soft |
| Main | Opaque → cut parts → caps → sheet at far depth → blended. Ends with a tile resolve of ID and Z to linear CPU-readable memory |
| AO (Metal, Clay) | Half-res compute, R8 |
| Composite | Edge quads into MAX-blended ink coverage, then one full-screen draw for outlines, AO, grain |

All scene attachments are 4× MSAA memoryless. Above 200 Mtri in a frame, or on the "too much geometry" error, depth switches to stored.

**HSR rules:** no `discard` or depth output in the opaque path; any discard or depth-writing PSO (a section's caps) draws after opaque; no depth pre-pass; clip planes apply only to parts crossing a plane (AABB test); keep varyings small.

**Geometry:** object + mesh shaders with thread-per-primitive threadgroups, in batches of 4096 or fewer (one direct dispatch of 120k silently drew nothing). Depth-only mesh PSOs need a stub fragment function. Per-meshlet frustum and cone culling. A background thread keeps a coarse front-to-back order, worth up to 6× on sub-pixel scenes. Hide a face by collapsing its vertices, since faces never share vertices. LOD and HZB occlusion are deferred unless spike 5 fails.

**Hover:** `globalFace` as a flat per-vertex value (free, vs +21% for `primitive_id` and +61% for per-primitive attributes). Hover is a uniform compare with no buffer write. Mouse moves with no target change cost no GPU work. Selection is one store. Hover edges draw through a face-to-edge range table.

**Picking and pivot:** read ID and Z from linear memory, with a small ring of camera matrices to unproject. A cap bit triggers an exact CPU ray-plane hit. Ghosts are click-through and write no ID, except while G is held, when ghosts write ID so a click can restore them (no second layer needed). A **meshlet-leaf BVH** provides visible bounds, section-clipped bounds and exact hit points (about 5 MB vs about 80 MB for a triangle BVH, estimates).

| Feature | Technique |
|---|---|
| 5 styles × 4 colour modes | Data tables, not code. Band and physical lighting are function constants; a colour mode rewrites a palette buffer. About 30 precompiled PSOs |
| Outlines | ID-based with an 8-ring early-out (+0.2–0.6 ms); the 24-read path at idle |
| Feature edges | Instanced quads, hand depth test; ghost dashes 4 on, 3 off using true arc length |
| Ghost shell/lines/both | Simple alpha with depth write off (+0.32 ms). Order error is at most about 8/255 at α=.24. WBOIT is the fallback at +0.40 ms |
| See-through | One pass with viewport depth ranges, bit-identical to 3 passes and about 35% cheaper. Layers: far walls `.5k`, opaque inner, shell `1-.8k`, hull ink `.6k`. Alpha via `setBlendColor` over 0.7 s |
| Shell vs inner | Automatic exposure/occlusion classification, cached in the blob, with per-part override (G can serve as the override) |
| Section | Up to 6 planes with `clip_distance`; back faces shaded as caps, chosen by each part's count of the surfaces behind the cut and drawn on the plane (ADR 0010); hatching in plane space, per-part variants; `open` flag for non-watertight parts. Union cuts use discard, drawn last |
| AO, shadow, grain, grid, wobble | All kept: they are part of step_viewer's schemes. AO uses 8 samples in motion, 12 at idle. Wobble maps the cursor through the same noise |

**Colour:** BGRA8 unorm, composited in encoded space, so reference hex values match. Only the physical path applies gamma.

**Frame pacing:** `maximumDrawableCount = 2`, one frame in flight. Render on demand with zero idle wakeups. Keep presenting briefly after input so ProMotion doesn't downclock. Draw view cube, scale bar, pivot ring and section gizmo in Metal for lockstep with the camera. M0 chooses the present path by measurement. `CAMetalDisplayLink` is the default candidate.

## Interaction

**Default mapping** (table-driven from M1, all rebindable, no key hard-coded):

| Input | Action |
|---|---|
| Left click | Select in the current mode (face/body/part) |
| Left drag (5 px threshold) | Rotate |
| Wheel | Zoom toward the cursor, anchor locked per burst, no inertia |
| Middle drag | Pan, 1:1 at the press-hit depth |
| Right drag / right click | Dolly, as in proto / context menu |
| Hold H + click | Hide (idempotent, no refit, re-pick at cursor so repeat clicks peel layers) |
| Hold G + click | Ghost; click a ghost to restore |
| Shift or Space + left drag, pinch | Pan and zoom for trackpads |
| F, I, Shift+H, Shift+G, Esc, Cmd+Z, ? | Fit, isolate, show all, unghost all, clear, undo, shortcuts |

**Adaptive rotation**, keeping proto's feel:
- Free trackball, no world-up lock, no inertia. `dxN=2dx/W`, `dyN=-2dy/W` (both over width), `angle=hypot·3.0`.
- Rotate position, target and up about the pivot, so panned framing is preserved.
- Pivot resolved once per drag. Modes: adaptive (default), visible, pointer, plus explicit "set rotation center".
- Adaptive: close-up if any bounds corner is behind the camera or NDC size > 1.5; exit below 1.2, keeping prior state between. Close-up samples 9 points of {-.2,0,.2}² around the **viewport centre**; otherwise the pivot is the centre of visible bounds.
- Only pointer mode uses the press position (a correction to the common reading of proto).
- Re-evaluate only on pan, zoom, fit or visibility change. Anchors store occurrence, face and local point, and hidden or sectioned anchors fall back to visible bounds.
- Ortho defines "behind" as a near-plane test, since proto's test covers perspective only.

**Camera:** one pose (quaternion, f64 position, focus distance, ortho height), rebased to f32 relative to the eye. Reversed-Z with infinite far. Orthographic default, as in step_viewer and colorfigures, with a perspective toggle. Flights 450–650 ms with slerp and log distance, instant under reduced motion. Discrete-wheel smoothing exists as a setting, off by default.

**Held keys:** ignore repeat and meta/ctrl/alt; stand down in text fields and modals; release on key-up, window blur or focus change. Match by logical character on key-down, then remember `keyCode` for key-up. Input mapping uses `Action`, `Trigger`, press-or-hold and scope, with inline conflict detection. System shortcuts are reserved.

## Visual design

The look is a printed-figure sticker, in the colorfigures palette. Paper `#f2efe7`, sheet `#f7f5ee`, ink `#231e16`, 1.5 px borders, hard offset shadows `3px 3px 0`, pill controls, IBM Plex Mono (bundled), ease `cubic-bezier(.3,.7,.2,1)` at 160/220 ms. Colours carry fixed meaning: cobalt = selection, cream = hover, tomato = about to hide, mustard = progress.

Layout: full-bleed canvas with a floating masthead, a 300 px left card (Library, Structure), a 286 px right selection card, a bottom-centred toolbar, top-right view cube and a scale bar. Chrome retints per style, so Blueprint doubles as the dark look in v1. Plain-sentence copy.

- Chrome uses `CALayer`, so UI changes never touch the `CAMetalLayer`. A test asserts zero 3D frames during UI-only changes.
- Hard shadows are a second rounded rectangle, with no blur or offscreen pass.
- Trees use a fixed-row `NSTableView` over an engine-owned flat array, spiked at 1M rows. Search scans a contiguous name blob with NEON.
- The external 3440×1440 display runs at 1× and 60 Hz: snap strokes to device pixels and raise the smallest type sizes.
- Open and save panels, menu bar and context menu stay native.

## Repository layout

```
geometryview/
  App/            Swift AppKit: Chrome, Design, Library, Structure, Settings, Bridge
  Engine/
    include/engine.h   the only public header (C ABI)
    base/ math/ scene/ model/ nav/ pick/ render/ ipc/
    gpu/          Objective-C++ Metal backend, FrameSource
    shaders/      .metal plus shared/ headers for C++ and MSL
  Import/
    format/       blob layout, shared by Scene and helper
    step/ stl/    parsers
    helper/       supervisor main
    occt/         the only code that includes OCCT headers
  Tools/gcli/     headless open, render, bench, blob dump
  Tests/          unit/ golden/ perf/ fuzz/
  ThirdParty/     pinned sources and manifest
  Docs/adr/       architecture decision records
  CMakePresets.json
```

Dependencies run one way: `base → math → scene → model → nav/pick → render → gpu → engine.h → App`. `Import` shares only `base` and `format`, enforced by per-target include paths.

**Build:** CMake + Ninja presets (debug, release, asan, tsan). OCCT needs CMake anyway, so one text-reviewable build system covers everything, and the Xcode generator stays available for Metal debugging. This differs from the red team's Xcode-project recommendation.

**Quality:** clang-format and swift-format; `-Werror`; `-Wunsafe-buffer-usage` on parsers; hardened libc++ in the helper; Swift 6 strict concurrency; ASan, UBSan and TSan runs; a malloc-count test asserting zero allocations per frame; fuzzing via Homebrew LLVM (tokenizer, STL, blob reader). Conventional Commits; every commit builds and passes tests. Sandbox and hardened runtime from day one, with file descriptors passed to the helper and security-scoped bookmarks for connected directories.

## Milestones

Each is a vertical slice. All budgets are targets. **Visual review pauses:** M1, M4, M6.

| # | Slice | Exit criteria |
|---|---|---|
| **M0** | Setup: `git init`, `brew install cmake ninja llvm`, `xcodebuild -downloadComponent MetalToolchain`. Skeleton, app shell, C API, mesh-shader test scene, one sticker card. Spikes 1, 3, 4, 9 | First frame ≤200 ms; zero idle wakeups; present paths measured on both displays; decisions in ADRs |
| **M1** | STL interactive: parsers, weld, pseudo-faces, cache, ID/Z, hover, adaptive rotation, zoom, pan, fit, input table | 10M-triangle STL at display rate; hover within 2 refreshes; proto test vector passes; zero allocations per frame |
| **M2** | STEP: tokenizer, tree, dedup, slicer, workers, streaming, refinement | 320-file equivalence check reproduced natively, including the void-shell fix; pack ≤0.1 s, 52.7 MB ≤1.6 s; helper crash leaves viewer alive; OCCT memory stays out of the viewer |
| **M3** | Assembly tree, instancing, selection modes, hold-H and hold-G, undo, search, multi-file scene | 100k-row tree at display rate, 1M-row spike done; search within a frame; hide and undo of 1M faces ≤1 ms |
| **M4** | Full visual language: controls, cards, masthead, toolbar, view cube, scale bar | Your design review; idle stays at zero; UI changes cause no 3D frames |
| **M5** | Five styles, four colour modes, edges, outlines, AO, shadow, grain, wobble, ghost shell/lines/both. Spikes 8, 10 | Golden matrix passes; composite and budget targets held |
| **M6** | See-through and section. Spikes 6, 7 | Reproduces the colorfigures look; caps correct on 20 CADBench models; your visual review |
| **M7** | Connected directories, FSEvents, bookmarks, eviction, settings, key and mouse rebinding | 97k-file tree loads on demand with no file reads; access survives relaunch; edited file reloads; all bindings editable; conflicts caught |
| **M8** | Tier 0 tessellator, FixShape experiment, spikes 2, 5, fuzz, sandbox review, perf gates | 792 MB file ≤0.5 s; 24 h fuzz clean; perf gate active |

**The gallery** (ADR 0017, 0018): whole folders of files in five views, built on the core above.
Plates and Sheet ship in 0.2.0, the rest in 0.3.0. **Review pauses:** G0, G2, G3, G4, and each
of G6 to G8.

| # | Slice | Exit criteria | State |
|---|---|---|---|
| **G0** | Pictures: render options, a clear sheet, folder colour, the thumbnailer, `gcli sheet`, async offscreen pictures in the browser | 20 goldens unchanged; six RGBA goldens; ≤6 ms a picture warm at 256 px; small pictures read as their parts | Done (2026-11-11) |
| **G1** | Foundation: facts, helper flags, the gate, the queue, the library service and its caches, scans, the browser's import queue, OPFS v2 and picture worker, the shell's `/scan`, ADR 0017 | S × W from a 500-file sweep; a file opened never waits behind gallery work; a second service starts no helper; prefetching does not evict Recent; node tests in CI | Done (2026-11-11) |
| **G2** | Plates on the Mac and the web: the layout core, painters, checks, ADR 0018 | 0 alignment groups over 0.5 pt; Mac and web within 1 pt; no 3D frames under the gallery; no web long task over 50 ms | Done (2026-11-11) |
| **G3** | Sheet, the view switch, shared state, flights, Peek, keys, menus | The selection survives every switch; motion matches the spec; G2's criteria still hold | Done (2026-11-11) |
| **G4** | States, FSEvents, phones, a poster PNG, MCP `view_folder` | iPhone ≤600 MB on test-models; no horizontal scroll at 390 px; a changed file re-inks alone; five tools | Done (2026-11-11); the iPhone at most 489 MB on test-models |
| **G5** | Release 0.2.0: Plates and Sheet | | Folded into 0.3.0: all five views were ready together |
| **G6–G8** | Ladders, Atlas, Catalogue | Each reviewed | Done (2026-11-11) |
| **G9** | Posters for every view, PDF, turnable Peek, release 0.3.0 | | Done; release 0.3.0 (2026-11-11) |

**Spikes** (pass/fail, recorded in ADRs):

| # | Spike | Pass | On failure |
|---|---|---|---|
| 1 | Mesh vs classic on real meshlets (850k, 10M samples) | Mesh no slower; fill ≥60% | Tune or merge meshlets; classic path if still slower |
| 2 | Same on M1/M2, if hardware is available | ≤1.5× classic | Lower quality on Apple7/8 |
| 3 | MTL4 encoders vs Metal 3 | Within 5% | Ship Metal 3 first |
| 4 | Residency: release, pressure, replaced cache file | No wired leak or crash | Copy to private memory |
| 5 | 30 Mtri, 60 s, battery and AC | p99 ≤8.33 ms | LOD, then HZB |
| 6 | See-through on coincident CAD faces | 0 double-blended pixels | Stencil one-shot |
| 7 | Caps on 20 models, multi-plane | No garbage | Real cap meshes on a background thread |
| 8 | Ink quality from sample-0 ID | Matches reference | Idle jitter accumulation |
| 9 | Input-to-photon | p95 ≤25 ms | Revisit latency settings. **Failed at 60 Hz:** p95 about 54 ms with a 33 ms system floor, 120 Hz untested (ADR 0005) |
| 10 | 5M edge segments | ≤1 ms | Meshlet-cull lines |

**Performance targets:**

| Fixture | Time to tree | First pixel | Complete (miss) | Cache hit | OCCT whole-file today (measured) |
|---|---|---|---|---|---|
| demo_drive 1.1 MB | ≤5 ms | ≤20 ms | ≤20 ms | <5 ms | 0.09 s, 329 MB |
| Pack 29 MB | ≤10 ms | ≤20 ms | ≤0.1 s | <10 ms | 2.62 s, 1.46 GB |
| Hard 52.7 MB | ≤12 ms | ≤30 ms | ≤1.6 s | <10 ms | 19.2 s, 3.62 GB |
| 792 MB | ≤180 ms | ≤250 ms | ≤0.5 s (Tier 0) | <15 ms | 134.6 s, 16.7 GB |
| Binary STL 500 MB | n/a | ≤250 ms | ≤1.5 s | <15 ms | n/a |

Viewer memory target: about 134 MB render targets at 3456×2234 plus under 150 MB other anonymous memory, with geometry in clean file-backed pages. While actively rendering, add about 240 MB of driver memory, which is released a few seconds after rendering stops (measured, see ADR 0003). Frame target: p99 ≤8.33 ms at 120 Hz on this machine.

**Deferred (not dropped):** measurement (face data stays in the blob), H/G drag painting, version-sibling files, PNG export, explode, turntable mode, thumbnails, dark theme beyond Blueprint, IGES, BREP, OBJ, PLY, 3MF, glTF, multi-window, Tier 1 cylinder/cone tessellator.

## Requirements coverage

| notes.md requirement | Milestone |
|---|---|
| STEP, STL, own files | M1, M2 |
| Connected directories with tree | M7 |
| Left select, wheel zoom, middle pan | M1, M3 |
| Hold H hide, hold G ghost, shell/lines/both | M3, M5 |
| See-through, colorfigures palette | M4, M6 |
| step_viewer colour and rendering schemes | M5 |
| Settings for mouse, keys, rendering | M7 |
| Hover highlight | M1 |
| Assembly hierarchy | M3 |
| Sectioning | M6 |
| Adaptive rotation | M1 |
| Atomic commits, quality | All |

## Risks

| Risk | Mitigation |
|---|---|
| Slicing fails on B-spline-heavy industrial STEP (no tested fixture has these) | Equivalence check per file in M2; whole-file OCCT fallback; acquire class-A samples |
| Real meshlets lose the mesh-shader advantage | Spike 1 first, classic path as fallback |
| Wired memory doesn't release (seen unresolved within 200 ms) | Spike 4 before cache format freezes |
| Dedup or slicing changes appearance vs OCCT | Signed-volume check, golden comparison |
| Style matrix explodes testing | Styles as data, golden matrix |
| Polish absorbs schedule | Finite control set built once, in M4 |
| About 30–40k lines of native code (estimate) | Vertical slices, each shippable |

## Key references (ideas only)

The author's earlier, private prototypes informed these choices. None of their code is copied.

- Rotation: an earlier prototype's adaptive orbit about the point under the pointer (test vector: 2-unit cube, fov 90, z=2.2 no, 1.6 yes, 1.75 stays, 2.0 exits)
- Styles and constants: step_viewer's render pipeline (styles, surface, composite, occlusion, lines, shadow light), its palette and its see-through classification
- Tokens: step_viewer's design tokens
- See-through: Colorfigures' layered see-through
- Ghost choices: an earlier prototype's face ghosting and inspection viewer, and its held keys
- **Do not copy:** step_viewer's dash code (it shadows `along`, confirmed by reading), its flat per-occurrence face numbering, or Colorfigures' alternative see-through path.

## Verification

- **Unit and property tests:** `ctest --preset debug` covers tokenizer, tree against XCAF ground truth, blob round trips, undo, camera test vectors.
- **Import equivalence:** `gcli` compares sliced output against whole-file OCCT on the 320-file set: face counts, area, signed volume.
- **Golden images:** `gcli render` across the style × mode matrix, with a perceptual tolerance.
- **Performance:** `gcli bench` on the fixtures, median of 5, plugged in, with a 5% regression gate on this Mac. The Metal HUD and `presentedTime` logs verify frame pacing; malloc-count tests verify zero per-frame allocation.
- **Robustness:** ASan, UBSan and TSan runs, parser fuzzing, helper kill tests.
- **End to end:** launch the app and walk `notes.md` line by line, using `demo_drive/detailed`, the 13S4P pack, the 52.7 MB file, the 500 MB binary STL, the 453 MB ASCII STL with its `color` line, and the CADBench directory. Real-file fixtures stay outside the repo, located by an environment variable, and small fixtures are checked in.
