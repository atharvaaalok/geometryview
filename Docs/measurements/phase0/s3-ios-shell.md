# S3: the iOS shell on an iPhone 16 Pro

Measured 2026-11-07 on an iPhone 16 Pro (iOS 26.7.2) with the shell in `iOS/Shell`. The census
page from `Web/census` ran inside WKWebView, served in two ways:
- **geo://**, the app's `WKURLSchemeHandler`;
- **loopback**, a small HTTP server in the app on 127.0.0.1 (`LoopbackServer.swift`), which never
  leaves the phone and needs no network permission.

Raw reports are in `census/ios/`. Memory runs end when iOS stops the page, so their last
`.partial.json` holds the result and a `.terminated.json` notes the stop.

## Verdict

**The thin shell passes its gate.** WebGPU works, and iOS grants well over the 1.1 GB the plan
required, so plan B (the Metal engine as a native iOS app) is not needed.

**The viewer must be served over loopback, not geo://.** WebKit honours the cross-origin isolation
headers only on HTTP responses, so a geo:// page has no SharedArrayBuffer and no WebAssembly threads.
On loopback the page is isolated, and threads work.

## Capabilities

| | geo:// | loopback |
|---|---|---|
| Cross-origin isolated, SharedArrayBuffer | no | **yes** |
| Shared WebAssembly memory posted to workers | DataCloneError | yes |
| Atomics.wait/notify hand-over between two workers | — | 2.1–2.3 µs (headless Chrome on the Mac: 2.8 µs) |
| Compiled module posted to a worker, instantiated there | yes | yes |
| `compileStreaming` in a worker | yes | yes |
| WebGPU, and WebGPU in a worker with OffscreenCanvas | yes | yes |
| OPFS sync access handles, 256 MB | write 0.5–1.0 GB/s, read 13–14 GB/s | same |
| 256 MB file streamed from the app | 1.6–3.4 GB/s, first byte 4–7 ms | 1.5–2.0 GB/s, first byte 100–140 ms |
| A small request | ≤2 ms | median 0.9 ms |
| Frame rate | 58.8 Hz | 58.8 Hz |
| `navigator.hardwareConcurrency` | 4 | 4 |

**WebAssembly:**
- has SIMD, both legacy and exnref exceptions, and tail calls;
- lacks relaxed SIMD, memory64 and JSPI.

**WebGPU:**
- features: timestamp-query, shader-f16, depth32float-stencil8, indirect-first-instance and float32 filtering;
- not offered: clip-distances, primitive-index, subgroups, transient attachments;
- limits: maxBufferSize and maxStorageBufferBindingSize are 1 GiB, with 44 storage buffers per stage, 32 KB of workgroup storage and 1024 invocations per workgroup;
- canvas format: bgra8unorm.

## Memory before iOS stops the page

| Run | Granted |
|---|---|
| GPU buffers, cleared, 256 MB steps (geo://) | 1792 MB |
| WebAssembly memory, touched, 64 MB steps (geo://) | 1984, 1920 MB |
| WebAssembly memory (loopback) | 1728 MB |
| Shared WebAssembly memory (loopback) | 1984, 1728, 1728 MB |
| Shared WebAssembly memory after a full census in the same page | the 4 GB maximum was refused at creation: `RangeError: Out of memory` |

iOS gives no warning first. The app never received a memory warning in any run, because the page
runs in WebKit's own process. The page has to keep within a budget of its own.

## What this decides

**The iPhone gets threads.** The shell serves the viewer from loopback, so a threaded engine and
import run on the phone as on desktops. The geo:// handler stays only as a fallback.

**Memory:**
- Budget about 1.2 GB for WebAssembly and GPU together. Each ceiling above was measured alone, and the limit looks shared: the GPU run, too, ended with iOS stopping the page's own process.
- A memory's maximum reserves address space when the memory is made, and that reservation can fail. So the runtime makes its memory with a descending ladder of maxima (4, 3, 2, 1.5, 1 GB), as the census now does, and keeps one memory per worker small.
- Since iOS warns nobody, the page trims itself as it nears its budget.

**Renderer:**
- Sections use the discard fallback, since there are no clip distances.
- Every render target costs real memory, since there are no transient attachments. This is the visibility-buffer design's case.
- Segments are capped at 256 MiB, well under the 1 GiB binding limit.
- There are no subgroups, so culling must not rely on them.

**Frame rate:** WKWebView renders at 60 Hz on this 120 Hz display. That is WebKit's default for
pages, with no public switch, so interaction on iPhone tops out at 60 fps.

**Open, for P6:**
- The loopback server's first byte on a large file arrives 100–140 ms late, while small requests take under a millisecond and throughput is 2 GB/s. Disabling TCP's small-packet delay barely changed it (136 → 116 ms), so it is likely WebKit buffering the start of a large response. Model files should stream in Range requests, and the cause should be found.
- File input from Files and the share sheet, served with Range.
- Recovery when the page's process is stopped mid-session.
