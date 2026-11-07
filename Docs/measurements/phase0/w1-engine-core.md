# W1: the engine core as WebAssembly

Measured 2026-11-07 on the M4 Max, Node 22, Emscripten 7.0.0 (Homebrew). `wasm-release` is
single-threaded; `wasm-threads` adds pthreads (16 workers) and mimalloc. Raw results are in
`w1-probe.json`; `Tools/probe` produced them.

## What builds

The portable layers compile to WebAssembly unchanged in behaviour:
- base, math, scene, model, nav and render;
- the STEP tokenizer, structure and definitions;
- the STL reader, mesh welding, the packer and edges;
- the cache writer and reader.

The `wasm-*` presets build them, the unit tests and `probe`. The import driver that runs OCCT in
child processes stays native until W2. Only small, local changes were needed:

| Change | Why |
|---|---|
| `base/simd.h` uses Apple's `<simd/simd.h>` on Apple and `base/simd_portable.h` elsewhere | Same names and layout. `simd_portable_test` compares them on a million random cases each: vectors and quaternions agree within 1e-12 relative, inverses within 1e-9 |
| `base/parallel` uses GCD on Apple and its own pool elsewhere; a loop where there are no threads | Pool claims carry their batch's generation, so a late worker cannot run an old batch's function on a new batch's index. `Concurrent` replaces the packer's direct `dispatch_group` |
| `nowSeconds` uses `steady_clock` off Apple | |
| The tokenizer gained SIMD128 and scalar scans beside NEON | Shift-xor stands in for PMULL's prefix-xor. NEON is unchanged |
| STL's slow float path uses `std::from_chars` | No `xlocale` |
| 64-bit sizes are narrowed explicitly where WebAssembly's `size_t` is 32-bit | A segment larger than the address space is refused |
| `base/xxhash.h` wraps xxHash | Emscripten's NEON emulation does not compile inside xxHash's `extern "C"` block, so xxHash runs scalar there |
| `parallelSort` takes a tie-breaker | See determinism below |

`Tools/check_layers.py` now fails if portable code includes Apple headers outside the few files
that choose between platforms.

## Tests

| Build | Unit tests |
|---|---|
| `wasm-release` (one thread) | 149 pass, 1 skipped (the ring test needs threads) |
| `wasm-threads` | 150 pass |

The name search budget is 8 ms with threads and 25 ms on one thread. One thread searched a million
names in 14 ms.

## Results against native

**STEP: tokenize, structure and definitions.** The hash covers every index array, node, body, face
use, occurrence transform, definition and chunk.

| File | Records | Native | Wasm, 1 thread | Wasm threads | Identical |
|---|---|---|---|---|---|
| demo_drive | 18k | 0.9 / 0.2 / 0.6 ms | 2.6 / 1.2 / 3.1 | 2.9 / 1.4 / 4.2 | yes |
| pack | 555k | 9.6 / 4.5 / 2.8 | 72 / 10.5 / 66 | 35 / 10.9 / 21 | yes |
| PLE120 | 398k | 8.0 / 2.1 / 6.6 | 53 / 5.6 / 64 | 29 / 5.8 / 27 | yes |
| hard 52.7 MB | 707k | 9.0 / 3.9 / 123 | 85 / 10 / 212 | **37** / 10 / 210 | yes |

**STL: 10M triangles, 500 MB binary.**

| Build | Total | Parse | Pack |
|---|---|---|---|
| Native | 1.37 s | 0.04 | 0.70 |
| Wasm threads | 3.76 s (2.7×) | 0.06 | 1.92 (**2.7×**) |
| Wasm, 1 thread | 18.9 s | 0.08 | 13.7 |

Stages where the threaded build loses most, from `GEOMETRY_TRACE`:

| Stage | Native | Wasm threads | Ratio |
|---|---|---|---|
| Union by crease | 21 ms | 547 ms | 26× |
| Sort triangles | 43 ms | 249 ms | 6× |
| Build meshlets | 548 ms | 1067 ms | 2× |
| Open sides (edges) | 442 ms | 1180 ms | 2.7× |

WebAssembly has only sequentially consistent atomics, so each compare-and-swap in the lock-free
union-find is a full barrier, and on a mesh whose largest smoothing group holds 94% of triangles
the threads contend on a few roots. Skipping path-halving writes that change nothing cut the stage
from 709 ms to 547 ms; native is unchanged. mimalloc did not help, so allocation is not the limit.

## Determinism

`parallelSort` used to order equal keys however the standard library's sort happened to. Apple's
libc++ and Emscripten's newer one differ there, so the same STL produced differently ordered caches.
Triangles in one cell of a face, and edge sides sharing their ends, now break ties by index. Native
output no longer depends on the library, and native pack time is unchanged (0.70 s).

What still differs between native and WebAssembly is floating point below our code:
- Native ARM fuses `a*b+c` (FMA); WebAssembly cannot. Turning fusion off natively makes meshlets,
  triangles and edges identical.
- Even with fusion off, 1,099 of 7.7M vertices have an octahedral normal one unit apart in its
  16-bit encoding, from math-library-level differences.

Neither is visible. **Caches are therefore keyed per engine, native or WebAssembly, rather than made
bit-identical.**

## Verdict against the plan's gates

| Gate | Result |
|---|---|
| Portable unit tests pass under Node, single- and multi-threaded | **Pass** |
| Shim within 1e-12 of Apple simd on 1e6 cases | **Pass** (inverses 1e-9, as conditioning allows) |
| STL and STEP-structure output byte-identical to native | **Pass for STEP**. STL differs below our code, as above, so caches are per engine |
| Hard-file tokenize ≤40 ms | **Pass with threads** (37 ms); 85 ms on one thread |
| Threaded packer within 2× of native | **Fail: 2.7×**. Union-find and sort atomics are the cause, for P2 to restructure |
| Mac perf gate unchanged, 0 allocations per frame | See the ADR close-out run |
