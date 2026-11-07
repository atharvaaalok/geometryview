# ADR 0002: Map geometry from the cache file directly into Metal buffers

Status: accepted (spike 4, with one case untested)

## Context

The cache is meant to be mapped read-only and wrapped with `newBufferWithBytesNoCopy`, so geometry
exists once in memory and is backed by a clean file. Planning found that binding such a buffer wires
the whole mapping, and left open whether wired memory is ever released. A leak would break the
design, so spike 4 had to run before the format froze.

## Method

`Tools/spikes/residency.mm`, on a 500 MB `MAP_SHARED`, `PROT_READ` mapping with a warm page cache.
System wired memory is sampled every 500 ms, including a control period with nothing mapped. Each
buffer is really read by the GPU with a blit.

The control period matters. A first version of this spike had none, and normal system movement looked
like a leak of 360 MB.

## Results

- **System noise:** wired memory moves by about 150 MB with nothing mapped at all.
- **No leak:** over 3 map, use and release cycles, wired memory rose by up to about 690–820 MB while in
  use, then stayed inside the control band after release, with no growth between cycles.
- **Footprint:** `phys_footprint` grew by 1.2 MB at most. File-backed geometry is not charged to the
  process.
- **Wiring is transient:** wired memory fell back to the control band about 2 s after use, even with
  `requestResidency` still in effect.
- **Setup cost:** residency plus first GPU read took 11–24 ms with a warm page cache.

First GPU read after going idle, then the read straight after it:

| Buffer | ~1 s idle | ~4 s idle | Next read |
|---|---|---|---|
| 16 KB anonymous | 0.27–5.15 ms | 4.00–5.31 ms | 0.17–0.50 ms |
| 500 MB anonymous | 0.51–8.24 ms | 11.18–11.76 ms | 0.16–0.22 ms |
| 500 MB file-backed | 0.63–7.96 ms | 11.21–11.90 ms | 0.15–0.19 ms |

File-backed and anonymous memory cost the same. Even a tiny buffer pays about 4–5 ms, which points to
the GPU waking. The rest grows with allocation size, not with the kind of memory.

## Decision

- Keep zero-copy mapping: `MAP_SHARED`, `PROT_READ`, `MTLResourceStorageModeShared`, untracked hazards.
- Write cache files only through a temporary file and an atomic rename. An open mapping then keeps the
  old inode alive, and a file is never truncated underneath it, which would raise SIGBUS.
- Split geometry into chunks of 256 MB–1 GB as planned, so a scene can drop unused chunks.
- Expect the first frame after several idle seconds to take about 5–12 ms longer. On-demand rendering
  should start GPU work when input begins, not when hover first needs it.
- Exclude that first frame when judging input latency, and report it separately.

## Not tested

- **Memory pressure.** Forcing it would disturb the machine running the test.
- **A cache file replaced while mapped.** This relies on POSIX inode semantics and is not exercised.
- **Cold page cache.** Planning measured 174–185 ms for 500 MB cold.
