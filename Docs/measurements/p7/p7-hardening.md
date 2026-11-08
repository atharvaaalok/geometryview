# P7: hardening, first round

Done on 2026-11-07 (late evening, Pacific Time) on the M4 Max, while the iPhone was unreachable,
and on the iPhone 16 Pro early on 2026-11-08, once it could be reached again.

## The 1M-part rack's frame

The WebGPU renderer drew the rack of a million parts in 8.28 ms at 2880×1800, against 5.62 ms in
the Phase 0 spike.
- **The cause** was P3's 16-bit indices, which split each list into draws of 512 meshlets. A million
  one-meshlet parts made about 1,950 chunk draws in the main pass and as many for the shadow:
  about 3,900 indirect draws a frame, at about 0.7 µs each.
- **The fix:** a scene whose longest list would need more than 256 chunks now writes 32-bit indices
  that name their slots whole, and draws each list at once.

| Native Dawn, 2880×1800, one sample | Before | After | Buffers |
|---|---|---|---|
| Rack of 1M parts | 8.28 ms | 7.12 ms | 363 to 512 MB (indices twice as wide) |
| Rack of 100k parts | 3.94 ms | 3.96 ms | 36.4 MB, unchanged |
| 10M STL | 5.29 ms | 5.33 ms | 268.8 MB, unchanged |
| 13S4P pack | 1.21 ms | 1.21 ms | 5.7 MB |

**What remains, about 1.5 ms, is not yet traced.** The rack is still faster than Metal on the same
frames (19.35 ms with four samples).

**Segments uploaded from memory** (native tools, tests) now get room made for them all at once. The
100k rack's buffers had grown to 41.3 MB through the arena's headroom.

## Fuzzing the web paths

| Target | What it feeds | Runs, 5 min | Found |
|---|---|---|---|
| `fuzz_stream` (new) | untrusted cache files to `CacheStream`, in pieces of any size, placed or declined by a sink that traps on any write outside a placement | 294,000 | nothing |
| `fuzz_blob` | segments to `parseSegment`, rewritten in P4 over `parseSegmentParts` | 22.8 million | nothing |

**Writing `fuzz_stream` found a hole, now closed.** A segment's header could make the stream take
memory before any of its bytes arrived:
- GPU memory for a placement;
- CPU memory where the sink declined.

So a crafted file could ask for gigabytes. Two changes close it:
- a declined segment now grows as its bytes arrive;
- where the host knows the file's real size (a File's, or a Content-Length), the header must agree.
  That bounds what it can ask to be placed.

## Robustness

- **GPU timing could break every frame.** Metal allows only a few timing counter buffers across the
  whole system. With dozens of viewer tabs open, the renderer's query set could not be made,
  and every frame's commands failed with it: the browser showed nothing. The renderer now adopts the
  query set only once the GPU confirms it, and goes untimed otherwise.
- **A phone refuses an STL import that would pass about 1.2 GB**, with a way forward, rather than
  losing the page.

## Import memory

`GEOMETRY_TRACE` now shows the process's memory footprint and peak at each stage, on Apple systems.
The 10M STL, natively:

| Stage | Footprint | Peak so far |
|---|---|---|
| weld | 873 MB | 873 MB |
| match edges | 1,308 MB | 1,308 MB |
| grow regions | 1,433 MB | 1,433 MB |
| split vertices | 1,622 MB | 1,622 MB |
| normals | 1,927 MB | 1,927 MB |
| open sides (packer) | 2,419 MB | 2,419 MB |
| build meshlets | 1,463 MB | 2,430 MB |

That is about 250 bytes a triangle, and the browser's import peaks the same way.
- Freeing the soup and the indexed mesh as soon as the next stage has them did not lower the peak:
  the allocator reuses those pages for packing.
- Cutting it means reworking the regions and the packer's open sides. Until then, phones refuse
  very large STL files (above).

## The viewer, finished further

- **A STEP file shows while it imports.** Its segments go to the page as the importer writes them,
  as on the Mac, so an assembly fills in part by part. The hard file is a single part, so it still
  appears only at the end.
  - Import times in Chrome, with every triangle in place: the pack 0.56 s, the hard file 3.22 s.
  - The first version lost the segments written as the import finished, because the supervisor had
    closed its file by then: PLE120 showed 78,670 of its 116,024 triangles. The launcher's
    end-to-end test caught it, and now checks the exact count.
- **A still view's four samples are drawn one a frame**, each costing one sample, rather than all
  four in a frame a touch would wait for.
  - The fourth step leaves exactly what four at once draw: a Dawn test compares them, with no
    difference.
  - Phones, which skipped the four-sample frame when it took over 64 ms, now get it too.

## P6, the iOS shell, run on the phone

The shell now:
- opens into the viewer by default; `--runs` still makes the measurements;
- serves the viewer's folder, fonts included. It had served only the census and spike folders, so
  the viewer could not have loaded on the phone;
- opens STEP and STL files from Files and the share sheet, keeps them readable, and serves them to
  the page under random paths, with byte ranges;
- loads the page again and reopens the last document if iOS stops the page's process.

**On the iPhone 16 Pro, every check passed:**
- **The viewer loads**, in 50–62 ms from navigation to an engine ready to draw. The 10M STL fits
  in at most 630 MB, against the 900 MB gate (`p4/p4-runtime.md`, "The iPhone").
- **A document handed to the shell opens.** `devicectl` passed the app a STEP file's URL, as Files
  does, and the page imported PLE120 and showed all 37 parts.
- **Recovery:** with the pack open, its WebContent process was stopped (SIGKILL). The shell
  loaded the page again in a new process, which reopened the pack from the library within 6 s,
  without importing it again.
- **Imports on the phone:** the pack in 2.0–2.3 s, PLE120 in 3.4 s, the hard file in 19.9 s, each
  shown as it imported, with every triangle.

**The phone showed four faults, now fixed:**
- **A STEP file shown while it imports lost its colours**, in every browser, not only on the
  phone. The pack drew all yellow until reopened from the library.
  - The WebGPU renderer reset every body's look whenever the scene synced. The core sends looks
    again only when the model changes, and a progressive import syncs the scene as each segment
    arrives. Every part fell back to the first look.
  - The renderer now keeps its looks across syncs, as the Metal renderer does. A Dawn test syncs
    the scene again and checks that the picture holds; it failed before the fix, with 13% of
    pixels changed.
- **The toolbar squeezed its buttons on the phone.** Pills set a `min-width`, which let them
  shrink in the scrolling toolbar, so Isolate became a circle with its title spilling out.
  - Tools now keep their size, and the row scrolls.
  - A phone's toolbar drops Hide, Ghost and Isolate, which the selection card has, and the
    shortcuts button.
- **"1 parts · 1 bodies"** in the masthead, in the Mac app too. It now reads "1 part · 1 body".
- **The import check's report was named after `step/pack.step`.** The slash made the shell write
  the report into a folder that does not exist. Report names now use a dash.
