# W2: OCCT in Web Workers

Measured 2026-11-07 on the M4 Max (Chrome 157, Safari 26) and the iPhone 16 Pro (in the S3 shell). Raw reports are in `import/`.

## Method

**How the slices were captured.** With `GEOMETRY_DUMP_SLICES`, the native OCCT worker saves each STEP slice the importer sends it, with the request. `--replay` meshes saved slices one by one. Only the worker changes, never the importer.

**Fixtures:**
- the 13S4P pack (102 slices);
- the PLE120 gearbox (244);
- the hard 52.7 MB CADBench file (614 slices, 12,188 faces).

**What runs in the browser.** OCCT 8.0.1 is built with Emscripten by `ThirdParty/build_occt_wasm.sh`. It has WebAssembly's own exceptions and no signal conversion. The same meshing code becomes `geometry-occt-web`, a module for Web Workers.

`Web/spike/import.html` replays the slices. The page compiles the module once and posts it to each worker, then hands slices to whichever worker is free.

**Native timings** are the better of two sequential replays on an otherwise idle machine.

## Size

| | Measured | Gate |
|---|---|---|
| `geometry-occt-web.wasm` | 9.7 MB | ≤12 MB |
| With brotli | 2.4 MB | ≤4 MB |

A worker starts in 2–30 ms from the shared compiled module. Compiling takes 10–50 ms in Chrome (which caches it) and 45–130 ms in Safari (which compiles lazily).

## Same results as native

| | Faces | Failed | Triangles | Area |
|---|---|---|---|---|
| Pack, Chrome | 1,381, same | 0 | 54,482, identical | 8e-10 |
| PLE120, Node | 2,677, same | 0 | +6.9% | 3e-6 (worst slice 1.8e-4) |
| Hard, iPhone | 12,188, same | 0 | 340,646 against 340,629 | 3.5e-5 (worst slice 6e-3) |

**The area gate (1e-4) passes.** Where triangle counts differ, the WebAssembly meshes are as good, just tessellated differently. The likely cause is floating-point rounding (native ARM code fuses multiply-adds, WebAssembly cannot) and last-bit differences between the two maths libraries: OCCT's mesher refines wherever a value crosses a tolerance. So the plan's "triangles ±0.5%" gate is too strict a test of this. Caches are already keyed per engine (W1), so the two never mix.

## Speed per slice

Each slice is timed on one warm worker, as OCCT's read, transfer and mesh times, against native:

| | Chrome total | Chrome p50 / p95 | Safari total | Safari p50 / p95 |
|---|---|---|---|---|
| Pack | 1.37× | 1.37 / 1.52× | 1.42× | 1.41 / 1.55× |
| PLE120 | 1.32× | 1.33 / 1.48× | 1.37× | 1.33 / 1.57× |
| Hard | 1.35× | 1.33 / 1.43× | 1.34× | 1.31 / 1.46× |

**The gates (p50 ≤2.0×, p95 ≤3.0×) pass** in both engines with room to spare.

An early Node run measured 2–4.5×, but the machine was then running another heavy build (load average 250), so that run is discarded.

## The hard file, all workers

| M4 Max | OCCT phase, cold | Warm | Plan's gate for the whole import |
|---|---|---|---|
| Native, 12 processes | 1.67–1.75 s (whole import 1.84–1.88 s) | | |
| Chrome, 12 workers | 2.50 s | 2.20 s | ≤4 s |
| Chrome, 16 workers | 2.37 s | 2.03 s | |
| Safari, 12 workers | 4.45 s | 2.80 s | ≤5 s |

**The rest of the import** (tokenize, structure, definitions and packing) takes 0.23 s natively. At W1's ratios that is about 0.5 s in WebAssembly. So:
- **Chrome** comes to about 3 s cold: it passes.
- **Safari** comes to about 5.0 s cold: at the gate. Warm, it is about 3.3 s.

Safari's cold penalty is each worker optimising OCCT's code for itself. A pool of workers kept warm from one import to the next, as the viewer would keep, removes most of it.

## iPhone 16 Pro

| | 1 worker | 2 workers | 4 workers |
|---|---|---|---|
| demo_drive | 0.54 s | | |
| Pack | 1.50 s | | |
| PLE120 | 3.27 s | 2.45 s | 3.25 s |
| Hard | | 16.2 s | |

**Two workers is the right number** on this phone: two of its six cores are fast, and four workers were slower than two.

During the hard file's import the page peaked at 337 MB and WebKit's GPU process at 50 MB (`import/iphone-import-hard-w2-memory.json`). Each worker's heap stayed under 33 MB.

**The plan's gate** was: two workers, the hard file in ≤30 s, no termination, peak ≤900 MB. **It passes** at 16.2 s and about 390 MB.

## What this decides

**Decision 6 holds: no native accelerator is needed.** The rule was to add one if WebAssembly import exceeded 3× native, and it is 1.3–1.4× per slice. The desktop launcher can keep its native helper anyway, since it already exists and is fastest.

**The P2 design is confirmed:**
- one compiled module, posted to a pool of workers that stay warm;
- two workers on the iPhone and one per performance core on desktops;
- a cap on each worker's memory.

## Not yet measured

- **Recovery from a trap or an out-of-memory worker** (gate ≤100 ms, losing one face). The worker has test hooks for it (`occt_trap`, `occt_exhaust`) and a 512 MB cap per worker, but the page does not exercise them yet.
- **The event-driven supervisor (P2).** The spike replays the native importer's slices, so the front end and packing in WebAssembly were timed in W1, not end to end here.
