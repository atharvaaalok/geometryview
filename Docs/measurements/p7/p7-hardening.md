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

**The rest was in the cull, and is gone** (branch `p7-rack-frame`, 2026-11-08). Instruments'
Metal System Trace timed the native bench's passes. Dawn merges consecutive compute passes into one
Metal encoder, so a small buffer clear between the cull's dispatches was needed to time each:

| The 1M rack's cull | Before | After |
|---|---|---|
| Count | 0.21 ms | 0.22 ms |
| Scan: one workgroup over the workgroups' counts | 1.45 ms | under 0.44 ms |
| Write slots and indices | 2.02 ms | 1.46 ms |
| Chunk draws, edges | 0.20 ms | 0.20 ms |

- **The scan** went 256 workgroups at a time, through fourteen series of counts: some 14,500
  barriers for 16,000 workgroups.
  - Now each thread sums a run of workgroups, the 256 sums are scanned, and each thread numbers its
    run.
  - A workgroup's fourteen counts lie together, so each run is read once for all of them.
- **Writing indices:** the workgroup writes its meshlets' indices together, a meshlet at a time.
  - Every thread loaded each meshlet anew, through four dependent reads.
  - The thread that owns a meshlet now leaves what the others need in workgroup memory.
- **Tried and left out:** spreading every index evenly over the threads, so that none idle on
  small meshlets. Its bookkeeping cost more than it saved (5.40 against 5.31 ms).

| Native Dawn, 2880×1800, one sample | P3 | Round 1 | Now |
|---|---|---|---|
| Rack of 1M parts | 8.28 ms | 7.12 ms | **5.29 ms** (the spike: 5.62 ms) |
| Rack of 100k parts | 3.94 ms | 3.96 ms | 3.77 ms |
| 10M STL | 5.29 ms | 5.33 ms | 5.23 ms |
| 13S4P pack | 1.21 ms | 1.21 ms | 1.18 ms |

**In Chrome**, with the renderer bench page at its three zooms, the 1M rack went from 8.78 / 8.10
/ 7.35 ms to 7.26 / 6.95 / 6.29 ms, and 6.45 / 6.43 / 5.87 ms in a second run (`renderer/`).
Other work on the machine was using the GPU at the time, so runs varied by about 0.8 ms.

The main pass's vertex stage, 2.23 ms for 12M triangles, is now the frame's largest part. The rack
is far faster than Metal on the same frames (19.35 ms with four samples).

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

The 10M STL took about 250 bytes a triangle to import, natively and in the browser. A second round
on branch `p7-import-memory` (2026-11-08, about 3 AM Pacific Time) halved the browser's peak, with
the cache file byte for byte the same and no import slower.

| 10M STL | Before | After |
|---|---|---|
| Browser importer's heap, Chrome | 2,595 MB | 1,269–1,366 MB |
| Import in Chrome | 2.47 s | 2.39–2.45 s |
| Native heap in use, at stage ends | up to 1,276 MB | up to 878 MB |
| Native footprint, peak | 2,426 MB | 2,001 MB |
| Native import, 8 runs alternating with `main` | 1.300 s median | 1.285 s median |

The machine's load average was 6–45 throughout, so times were taken in alternating runs.

**What changed:**
- **The browser reads a binary STL file in pieces.** The importer had copied the whole file into
  its heap, where it lay under every later stage: 500 MB here.
  - `StlPieces` takes a binary file's records as 16 MB pieces arrive, and `ContentHash` hashes them
    as they pass.
  - ASCII and malformed files are still read whole.
  - Tests check that pieces of any size make `readStl`'s soup and the whole-file hash.
- **Each stage frees its input once the next has it:**
  - the soup once welded;
  - the edge table once regions are grown;
  - the welded mesh once vertices are split;
  - the sorted triangles once meshlets are built;
  - each packing unit's output as it is gathered;
  - the open-side lists as they are used.
- **Normals are summed a million triangles at a time**, in the same order, so they are unchanged.
  The scratch was 320 MB.
- **Dedupe keeps 32 bits of each item's hash**, not 64. Ids depend only on partition and order,
  so they are the same, and the array is half the size: 120 MB for 30M corners, in each of three
  passes.
- **Packing units size their outputs exactly once**, rather than growing a push at a time.
- **Threaded WebAssembly builds use dlmalloc rather than mimalloc.** WebAssembly memory never
  shrinks, so memory an allocator leaves unused raises the peak for good.
  - mimalloc had been chosen when the parallel stages allocated far more.
  - Six alternating runs each found no difference in time: 2.38 s against 2.40 s for 10M
    triangles, 1.40 s against 1.39 s for 5M.

**Two faults turned up on the way:**
- Assigning `{}` to a `std::vector` keeps its memory. The browser importer's "free the file" did
  not free it.
- On Apple systems, the footprint counts memory malloc has freed but kept. The trace now shows the
  heap's bytes in use beside it, and in WebAssembly the heap's size.

**The browser importer's heap by size**, under Node, reading in pieces:

| Triangles | mimalloc, before the last two changes | Now |
|---|---|---|
| 2M | 634 MB | 459 MB |
| 5M | 1,170–1,298 MB | 866–871 MB |
| 10M | 1,690–1,710 MB | 1,230–1,370 MB |

**Phones:** 300 MB and 115 bytes a triangle bound every import measured, so a phone now takes
binary STL files up to about 8.5 million triangles, where it took 4 million.

On the iPhone 16 Pro, a 5M-triangle STL (the 10M fixture's first half, 250 MB) imported in 3.1 s:
- the importer's heap reached 573 MB, less than under Node, with fewer threads;
- Instruments recorded the page's process at 893 MB at its peak and the GPU process at 217 MB
  (`p4/runtime/iphone-p7-import-stl5m-memory.json`).

**Near the limit**, an 8.4M-triangle STL (the 10M fixture's first 8.4M, 420 MB) imported on the
iPhone in 4.9 s, with no error:
- the importer's heap reached 741–845 MB;
- the page's process peaked at 1,271 MB and the GPU process at 232 MB
  (`p4/runtime/iphone-p7-import-stl8m-memory.json`). The limit's estimate for this file,
  300 MB + 115 B a triangle, is 1,266 MB: it predicts the page's peak.
- iOS stopped pages at 1.7–2.0 GB in Phase 0, so this leaves 400–700 MB to spare, for the GPU
  process and whatever else the phone is running.

**Still to do:**
- The 10M import's heap still grows at packing. The 160 MB sort and its 160 MB radix scratch do
  not fit the holes the earlier stages leave in a heap that cannot shrink.
- The native footprint is still two-thirds memory malloc has freed and kept. Native imports run in
  their own helper process, which ends, so this matters less there.

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
- **A STEP file refines as the view comes close, as on the Mac** (branch `p7-web-refinement`).
  - **Before:** the browser refused every refinement, so a close view kept the coarsest
    tessellation.
  - **The request:** when the settled view is close, the engine asks for some definitions at a
    finer level.
  - **The work:** the page imports them again from the STEP file, in the importer worker, on its
    warm OCCT pool and behind any import. It hands each segment to the engine as it is written,
    and the segment joins the open document.
  - **The source:** it is the file imported this visit, or the one the library keeps beside its
    cache. The library writes it after the import is done, and uses it only if its size matches
    the import's note. A file reopened from the library refines too, as does one the iOS shell
    reopens after its page was stopped.
  - **Phones** refine to level 2, a quarter of the coarsest chord error, rather than 4, since
    refined geometry stays on the GPU beside the coarser.
  - **On the iPhone**, PLE120 refined from 116,024 to 219,720 triangles, complete 2.7 s after the
    view settled, with two workers. The page's process peaked at 515 MB and the GPU process at
    105 MB, well inside the phone's budget.
  - **The hard file on the iPhone** refined from 340,646 to 589,673 triangles, drawn 21.4 s after
    the zoom (2026-11-08, 4:23 PM). That was one job, its one definition to level 1, with two
    workers, no crashed worker and no failed face.
    - **Memory:** the page's process peaked at 835 MB as the job ended, and settled at 566 MB.
      The GPU process peaked at 136 MB as the scene loaded, and was at 120 MB as the job ended
      (`p4/runtime/iphone-p7-refine-hard-memory.json`).
      - The import alone peaks at 570 MB (P4), so the job takes the page 265 MB higher, for a
        moment.
      - That peak is still well inside what the phone allowed the 8.4M-triangle STL (1,271 MB).
      - The 270 MB that go at the job's end are most likely the job's own importer, which reads
        the whole file again and is let go when the job ends.
    - **In Chrome with the phone's settings** (two workers, level 2), the same job takes 12.0 s,
      so the phone takes 1.8× as long.
    - **The first runs showed no refinement, and the fault was in the check, not the viewer.**
      - First, it waited only 6 s for the count to change. It now waits while a job runs.
      - Then it stopped as soon as the job ended. The count changes only when a frame draws the
        new segment, so it could stop just before that frame. That run's job was done and
        reported no new triangles. The check now waits 3 s after a job too.
      - Each job in the report now says what the importer made (triangles, failed faces, crashed
        workers), how many segments it sent, and how many the page refused.
  - **Measured** in Chrome with the scripted check, `test=refine` (six wheel steps in on the
    middle):

    | File | Triangles | Complete after the view settled |
    |---|---|---|
    | PLE120 | 116,024 to 375,198 | 0.7 s |
    | The hard file | 340,646 to 967,300 | 3.9 s |
    | PLE120 in Safari, a window of another size | 116,024 to 388,092 | 1.1 s |

    The Safari run is also the new cull's first under WebKit, with no errors (`viewer/`).

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

## Linux and Windows

No physical Linux or Windows machine is at hand. Linux runs first in Docker on this Mac, without a
GPU; drawing on either system needs a machine with one.

**Tools** (`Tools/platforms/`):
- `headless.mjs` opens a check page in Chrome or Chromium on any system, waits for the report it
  posts, and prints it. It needs no packages.
- `linux.Dockerfile` is Debian 12 with Node 22, Debian's Chromium 156 (arm64), Mesa's lavapipe
  and Chromium's SwiftShader. `linux-run.sh` starts the census and viewer servers inside it and
  runs one check.
- The launcher's end-to-end test takes `GEOMETRY_CHROME` and `GEOMETRY_CHROME_FLAGS`, with
  Chrome's usual place on each system as the default.
- `browser-run.ps1` opens a check page in any browser on Windows, Firefox included, and waits for
  the report the page posts. It is the AWS runs' script, with their fixed paths taken out.

**Linux in Docker, Chromium 156 with SwiftShader:**
- **The census:** cross-origin isolation, SharedArrayBuffer, threads (a hand-over in 11 µs), a
  module posted to a worker, OPFS with sync access handles (1.4 GB/s write), and every WebAssembly
  feature the viewer uses, memory64 and JSPI included. The rAF rate is 60 Hz
  (`phase0/census/linux-docker-chromium-swiftshader.json`).
- **The launcher's tests:** 8 of 9 pass on Linux, the MCP server and the web server's guards
  included; one needs the native engine and is skipped. In the end-to-end test the page opened
  PLE120 from the launcher, imported it with OCCT in workers to exactly the Mac's 37 parts, 89
  bodies and 116,024 triangles, and listed its structure. It then failed at the picture.
- **The viewer** opens the pack (331,520 triangles), its interface complete, with 0 frames drawn
  while still. The 3D view stays blank.
- **Why nothing draws:** Chromium without a GPU, in a container, cannot present WebGPU to a
  canvas. Its GPU process reports no shared-image backing for the WebGPU swap chain, after which
  the page's WebGPU instance is lost, and reading a picture back fails too ("A valid external
  Instance reference no longer exists"). This held headless and under Xvfb, with SwiftShader or
  lavapipe, and with ANGLE on SwiftShader or Vulkan, Graphite, in-process GPU, and GPU
  compositing off. It is a property of that setup, not of the viewer.
- **A fix it led to:** a picture that fails now tells the AI app why, rather than only that it
  failed.

**On AWS, with a GPU.** Two g4dn.xlarge instances in us-east-2: an NVIDIA Tesla T4 and 4 vCPUs of
a 2.5 GHz Xeon each.
- **Windows:** Server 2025 with NVIDIA's GRID driver 596.86.
- **Linux:** Ubuntu 22.04 with NVIDIA's 595.112.
- **Software on both:** Chrome 157, Firefox 159 and Node 22.23; Edge is the Windows default.
- **Cleanup:** the instances, their security group and key were removed afterwards.

**How they were set up.** A browser on a server's desktop must match a PC's for the result to
mean anything:
- **Headless Chrome, with no desktop:** in Windows' service session, or on Linux without X, the
  engine ran but nothing was presented.
  - On Windows the GPU process crashed once at start.
  - On Linux there was no shared-image path for the WebGPU swap chain.
- **Windows:** checks run on the signed-in desktop, through a scheduled task.
  - The instance's display was a basic display adapter, with the T4 render-only. There Chrome
    found no adapter.
  - Disabling the basic adapter, as is usual on a GRID instance, made the T4 drive the desktop,
    as a PC's GPU does.
- **Linux:** an X server runs on the T4 (Xorg with NVIDIA's driver, a virtual 1920×1200 screen).

**The renderer** at 2880×1800, three frames a refresh, GPU ms p50 at the three zooms (`p3/renderer/`):

| Scene | Windows, Chrome (D3D12) | Windows, Firefox 159 | Linux, Chrome (Vulkan) | Mac M4 Max, Chrome (Metal) |
|---|---|---|---|---|
| 13S4P pack | 2.25 / 2.67 / 2.47 | 3.56 / 3.80 / 3.51 | 2.82 / 3.18 / 3.20 | 2.00 / 3.99 / 3.35 |
| 10M STL | 7.39 / 6.32 / 5.30 | 10.17 / 8.88 / 6.96 | 7.32 / 6.46 / 5.34 | 6.18 / 4.70 / 3.48 |
| 1M-part rack | 8.66 / 7.85 / 7.63 | 11.05 / 10.72 / 10.39 | 8.78 / 8.27 / 7.44 | 6.02 / 6.40 / 6.27 |

- **Load:** the Mac ran with a load average of 7–14 from other work. Even so, a T4, a 2018 data-centre
  GPU, draws every scene well inside a 60 Hz frame.
- **Firefox:** its WebGPU (wgpu, not Chrome's Dawn) takes about 1.4× Chrome's time on the same GPU.
  It slows a covered window's frames to a crawl: with the launcher's Edge window in front, its
  first runs drew one frame in six seconds.
- **The picture:** one fixed frame of each scene was read back on each system and compared with the
  Mac's. In Chrome on Windows and Linux, and in Firefox, at most 0.002% of pixels are more than 8
  levels off (0% for the pack and the rack), against the plan's gate of 0.5%. The STL's few are
  scattered single pixels.

**The viewer**, in a window on each desktop:
- **The pack:** 0 frames while still, 118 of 120 while dragged, and hover picks answered. The
  pictures match the Mac's.
- **Imports, with two workers** (four vCPUs): Windows first, then Linux.

  | File | Windows | Linux |
  |---|---|---|
  | The pack | 4.9 s | 4.6 s |
  | PLE120 | 7.8 s | 7.0 s |
  | The hard file | 38.4 s | 37.1 s |

  - No face failed, and every triangle count is the Mac's.
  - These cores take about twice as long as the iPhone 16 Pro's: the hard file takes 19.9 s
    there, also with two workers.
- **Refinement:**
  - PLE120 went from 116,024 to 388,092 triangles, in 6.3 s on Windows and 5.1 s on Linux.
  - The hard file went to 967,300, in 67–68 s on each: level 1, then level 2.
- **Firefox 159 on Windows:**
  - The pack: 0 frames while still, 119 of 120 while dragged, hover picks answered.
  - It loads the cache in 1.5 s, where Chrome takes 0.2 s.
  - Imports: PLE120 in 6.4 s; the hard file in 50.1 s, run with its window covered.
  - Refinement: PLE120 refined to 213,900 triangles in 5.3 s. Firefox draws at one pixel per
    point there, so it needs a coarser level than Chrome at two.

**The launcher and its packages:**
- **Its tests:** on Windows, 10 pass and the one needing the Mac's native engine is skipped;
  likewise on Linux. This includes the end-to-end test: open PLE120, list it, picture it.
- **As an AI app uses it, over MCP:** `open_model`, then `render_view`, on PLE120.
  - On Windows the launcher opened the viewer in Edge, the default browser, and answered in 13.6 s
    with Edge's start.
  - On Linux it opened its own Chrome window, as below, in 8.2 s.
- **The `.mcpb`:** unpacked and run with Node, as Claude Desktop runs it, on both. It listed its
  four tools, opened the pack in 4.3–4.9 s and pictured it.
- **The npm package:** a global install and `npx` both answered. A local tarball needs
  `npx --package=<tarball> geometry-viewer`.

**What the runs found, and the fixes:**
- **Chrome on Linux has no adapter in roughly the first 200 ms after the browser starts.** A page
  that asks then gets none, and a moment later gets the GPU's. The launcher opens the viewer in a
  browser it has just started, so the engine now asks again for up to two seconds.
- **Stock Chrome 157 on Linux gives a page no WebGPU on the NVIDIA GPU.** With Vulkan on
  (`chrome://flags/#enable-vulkan`) it gets the T4, and no unsafe flag is needed.
  - On Linux the launcher now opens the viewer in a Chrome app window of its own, with its own
    profile and Vulkan on, since flags cannot reach a Chrome already running. Without Chrome or
    Chromium it opens the default browser.
  - Where WebGPU is missing, the viewer says how to turn Vulkan on.
- **Firefox 159 on Linux has no WebGPU at all.** The viewer says to open it in Chrome, Edge or
  Safari.
- **Two of the launcher's tests failed on Windows only:** a file URL's pathname used as a path, and
  a path written with `/`. Both are fixed; the shipped code was already right.
