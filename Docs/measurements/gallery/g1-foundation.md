# G1: the gallery's foundation

Measured on 2026-11-11 (morning, Pacific Time) on the M4 Max: 12 performance cores, 4 efficiency
cores, 64 GB. Chrome 157 ran headless, with WebGPU on Metal. The iPhone is measured when it is
free; see the end.

## Imports at once (S) and workers each (W)

`Tools/gallery_sweep.py build/fixtures/cadbench` ran the library service over the CADBench sample.
- The sample is 496 STEP files: 128 easy, 151 medium and 217 hard.
- They take 680 MB: 142 KB at the median, 3.6 MB at p90, 50 MB at most.
- Each run uses the release `gcli library`, from an empty cache: a scan, then every file wanted
  in view, in order, imported, measured and drawn at 256 px.
- Peak is the most memory `gcli`, its helpers and their OCCT workers held together, sampled
  every 0.2 s.

| S × W | Seconds | Files a minute | Peak | Failed |
|---|---|---|---|---|
| 1 × 12 | 245 | 122 | 1.5 GB | 0 |
| 2 × 3 | 133 | 224 | 1.4 GB | 0 |
| 2 × 6 | 127 | 234 | 1.5 GB | 0 |
| 3 × 4 | 97 | 306 | 2.0 GB | 0 |
| 4 × 2 | 86 | 345 | 1.8 GB | 0 |
| 4 × 3 | 80 | 372 | 1.8 GB | 0 |
| 12 × 1 | 64 | 468 | 2.3 GB | 0 |
| 6 × 2 | 63 | 473 | 2.6 GB | 0 |

- Throughput rises with imports at once, not with workers each. Most files are small, with too
  few slices to keep many workers busy, so one import of 12 workers is the slowest.
- At 12 workers in all, 6 × 2 and 12 × 1 are equal, and both are 3.9× faster than 1 × 12.
- 6 × 2 keeps two workers for a large file, so one large file in view does not take a single
  worker's time.
- The 496 files' cache files take 130 MB, 0.26 MB a file. Each file started about two helpers: a
  structure pass while it waited, then its import. The more imports at once, the fewer files
  wait long enough for a structure pass: 496 at 1 × 12, 399 at 12 × 1.
- The reports are in `sweep-cadbench.json`.

**Chosen: S = max(2, P / 2), W = 2**: 6 × 2 here, 12 workers, the performance cores. On a 4-core
M4 that is 2 × 2. Its peak, under 3 GB, is a fraction of any Mac's memory, and the gate holds
all gallery imports to the performance cores whatever S and W are.

## A file opened never waits behind gallery work

`service_test.cpp` ("a file being opened is not kept waiting, and gallery imports not in view make
way") fills both of a gate's slots with gallery imports that take 20 s, then opens a file.
- The open ticket is granted at once, in under 5 ms in the debug build.
- The import near the view is cancelled within the check's 3 s, and the one in view runs on.
- No gallery import starts until the file is open.

The engine's own imports and refinements hold open tickets.

## A second service on the same cache starts no helper

`service_test.cpp` ("a gallery's files get facts and pictures, and a second service on the same
cache starts no helper") runs a second service with a helper that cannot start. Every file's
facts and picture come from the gallery's cache: 4 look-ups, 0 helpers.

## A prefetch walk does not evict Recent

`service_test.cpp` covers this from two sides.
- Prefetching stops while cache files fill 70% of their budget. With recent files at 71%, only
  the file in view imports.
- When a walk overflows the budget, the gallery's own cache files are evicted, since they are
  dated a year back, and the files opened recently stay. Every picture is drawn before its cache
  file goes.

## Scanning

`gcli library` scanned the 496-file sample in 5 ms (warm). A tree is listed in batches of 256,
or every 50 ms while files are found.

## The browser

`node Tools/web_report.mjs "/viewer/?test=thumbs&label=mac-chrome&count=60"`
(`web/mac-chrome-g1-thumbs-none.json`) drew the drive and motor fixtures 60 times in the picture
worker. Each picture went from a cache file to an ImageBitmap and a PNG.

| First picture | p50 | p90 | Worker heap | Workers replaced |
|---|---|---|---|---|
| 1680 ms | 4.5 ms | 5.4 ms | 16 MB | 0 |

- The first picture includes starting the worker, compiling its module, asking for a device and
  building every pipeline.
- Later pictures cost about 2 ms more than G0's bench, for the part's ids, the conversion to
  straight alpha and the PNG.
- At 16 MB a picture of these fixtures, the 256 MB limit replaces the worker only after files
  far larger.

**The import worker** (`?test=worker`, `web/mac-chrome-g1-worker-hard.step.json`), on the 52 MB
file:
- A structure pass took 321 ms and posted one 16 KB model segment.
- An import cancelled at its first progress stopped 0.5 s after it started, with the slices it
  had out answered.

**The viewer's own checks** report as before, now through the importers and OPFS v2:
- `test=import` imported the pack in 493 ms.
- `test=library` imported a file from a connected folder in 493 ms, and reopened it from Recent
  in 10 ms, from the import kept under its key. The folder's walk listed `sub` and
  `sub/pack.step`, with either way of reaching folders.
- `test=refine` refined the pack from 331,520 to 471,648 triangles in 0.5 s.

**Node tests** in CI cover the queue's cases, keys and eviction choices, and the importers' lanes.

## The iPhone

On the iPhone 16 Pro (iOS 26.7), in the shell over loopback HTTP, at DPR 3, with the G1 build
installed on 2026-11-11 at about 9:19 AM PT. Reports are in `iphone/`.

**The picture worker**, with a GPU device of its own in a worker:

| Run | Pictures | First | p50 | p90 | Worker heap | Replaced |
|---|---|---|---|---|---|---|
| First launch | 120 | 950 ms | 6.7 ms | 7.5 ms | 16 MB | 0 |
| Long run | 4,000 | 41 ms | 7.6 ms | 8.5 ms | 16 MB | 0 |

- Shaders were cached after the first launch, so later first pictures took 41–45 ms.
- The heap stays at 16 MB over 4,000 pictures, and iOS never stopped the page.
- So the phone draws pictures in their own worker and device, as the desktop does. Each takes
  about 1.5× Chrome's time on the M4 Max.

**The shell's walk and the library** (`test=library`, shell backend):
- The folder walk went through `/scan`: `sub` and `sub/pack.step`.
- The pack imported from the connected folder in 1.8 s, and reopened from Recent in 114 ms, from
  the import kept under its key.

**The import worker** (`test=worker`) on the 52 MB file:
- A structure pass took 410 ms, for one 16 KB model segment.
- A cancel at the first progress stopped the import 1.1 s after it started.

**Memory.** Not recorded:
- Instruments' Activity Monitor dropped the phone ("Device disconnected") about a second into
  every recording made while the shell ran, though it recorded the idle phone.
- The page's own figures are steady: a 16 MB worker heap and no stop over 4,000 pictures.
- G4's iPhone gate (600 MB on test-models) measures the whole page with the gallery, and needs a
  recording that holds.
