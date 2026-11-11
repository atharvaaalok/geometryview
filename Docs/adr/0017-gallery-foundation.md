# ADR 0017: The gallery's foundation: pictures, the library service and its caches

Status: accepted (2026-11-11). G0 and G1 of the gallery plan.

## Context

The gallery shows whole folders of STEP and STL files: thousands of files, each a picture and a
few facts. The viewer was built to open one file at a time.
- An import ran the whole helper at full width, one file after another.
- A picture was a whole renderer set up and torn down, about 12 ms of overhead on the Mac.
- The import cache was the only cache, keyed for files opened, and evicted as one pool.
- In the browser, imports ran one after another on the page, and what was kept was named by bare
  file names, so two `part.step` files in two folders overwrote each other's cache file.

The gallery needs these to scale to a folder, without slowing down the file a person opens.

## Decisions

**1. Pictures are the engine's own, drawn by a kept renderer, on a clear sheet.**
- The thumbnailer (`geo_thumbnailer_*`) keeps one renderer for many files: 2.5 ms a picture
  at 256 px on the M4 Max, against 12 ms with a fresh renderer each time.
- A picture comes out premultiplied, with the part, its lines and its cast shadow. The sheet is
  drawn by the gallery from `geo_style_sheet`, so a row of pictures shares one sheet. Metal
  composites with two sources, which leaves every opaque frame as it was: the 20 goldens did
  not change. WebGPU carries alpha the same way.
- Lines are drawn for the picture's size (`line_scale`, clamp(cell / 320, 0.5, 1)), not scaled
  down from a large render. A one-part file takes its folder's colour (`first_color`).
- Each picture reports the part's box, from its pick ids, apart from its shadow. Parts stand on
  a row's baseline by it.

**2. Facts outlive cache files.**
- `scene::Facts` holds a file's box in millimetres, its triangles and faces as drawn, its parts,
  bodies and distinct bodies, the name inside it and its source hash.
- It is measured from a cache file: exactly from vertices where a placement rotates, from
  placed boxes where it only swaps axes.
- From a STEP file's structure alone (the helper's `--structure`), the box is estimated. A
  gallery lays a file out by it while the file waits to import.
- Facts, pictures and failures are kept as small files, so a folder seen before shows at once
  and imports nothing. Only a new style imports again, for files whose cache files were
  evicted.

**3. One library service per gallery, one gate per process.**
- `library::Service` takes the files a gallery wants, in bands: open, visible, near, prefetch. It
  gives each what it still needs: a look-up of facts and picture, then a structure pass, then an
  import, then a picture. Decisions are made on one thread; jobs run on threads of their own.
- `library::Queue` is the order, as pure policy. A job no longer wanted is dropped before it
  starts. Once started it runs on, since its result is kept, unless a job in view needs its
  slot. Only a file being opened or one in view cancels a running job.
- `library::ImportGate` counts OCCT workers. A gallery's imports together hold at most the
  performance cores. A file the person opens takes an open ticket, granted at once; while it is
  held, no gallery import starts, and those near the view or prefetched are cancelled.
- An advisory lock on the source makes a second process wait for the first's import rather than
  repeat it. A second service on the same cache starts no helper.
- Memory pressure runs one import at a time on a warning. On a critical warning it cancels
  imports and starts none.

**4. Imports run S at once with W workers each, chosen by measurement.**
- Gallery imports default to max(2, P / 2) at once with 2 workers each: on the M4 Max, 6 × 2,
  the 12 performance cores.
- See `Docs/measurements/gallery/g1-foundation.md`: on 496 CADBench files, 6 × 2 imports 3.9×
  faster than one import of 12 workers, with a peak under 3 GB. Throughput follows imports at
  once, since most files have too few slices for many workers; two each keep a large file from
  taking one worker's time.
- A file opened still gets one per performance core.

**5. The gallery's caches sit beside the import cache, each with its own budget.**
- `<cache>/gallery/facts/<K>.json` holds a file's facts, or why it failed.
- `<cache>/gallery/thumbs/<K>.<T>.png` holds a picture. Its note, `.json`, is written last and
  marks it whole.
- `<cache>/gallery/roots/<R>.json` holds a folder's last listing.
- K is the import cache's key, so an edited file is a new entry. T hashes the drawing spec and
  the engine's version.
- Eviction counts any suffix, and groups a key's files: a cache file with its refinements, or a
  file's pictures with their notes.
- Cache files the gallery imports are dated a year back, so they are evicted before any file
  the person opened.
- Prefetching stops while cache files fill 70% of their budget. Eviction runs at most every
  30 s and spares files open or in view.
- Pictures get min(free space / 100, 2 GB).
- Partial files older than two hours are swept at start.

**6. Folders are scanned natively and bounded.**
- `library::scan` walks with fts(3): no links, no hidden files, depth 32, 20,000 files, in
  batches of 256 or every 50 ms.
- Each folder follows what is under it, with its count. The listing is kept.
- The iPhone shell serves the same walk as NDJSON at `/scan/<id>/<path>?depth&limit`. It marks
  files that live only in iCloud, which are imported only when in view and confirmed.

**7. In the browser, the same policy, and storage keyed by where a file came from.**
- `import-queue.js` is `library::Queue` in JavaScript. Both run `Tests/data/gallery-queue.json`.
- `importers.js` runs a live lane, files opened one at a time, and a gallery lane. A file being
  opened cancels a refinement running.
- On a desktop the gallery has an importer of its own, which starts nothing while a file opens.
  On a phone one importer is shared, the gallery's job makes way, and nothing is prefetched.
- The import worker takes `cancel` and `structure`.
- OPFS v2 (`kept.js`, `library2/{blobs,notes,thumbs,sources}`) names entries by the first 128
  bits of SHA-256 over source, path, size, time and cache version. The note is written last.
- A file in a connected folder keeps no source copy: it is read from its folder again to refine.
- Eviction is least recently used, to a share of the browser's quota. Storage is asked to
  persist once a folder is connected. The first `library/` store is still read.
- Pictures are drawn in a worker of their own (`thumbs-worker.js`, `geometryview-thumbs-web`),
  with its own GPU device. It is replaced once its heap passes 256 MB.

## Consequences

- A folder of thousands of files can be shown without blocking the viewer. A file opened during
  a gallery's work starts at once.
- Pictures, facts and listings are new cache formats with versions of their own: Facts has
  `kVersion`, and picture keys hash the engine's version.
- A folder renamed imports again, since keys hash paths. An identity by inode is left for later.
- The browser's pictures arrive about 1.7 s after the worker starts, while its device and
  pipelines are built. After that each takes 4.5 ms.
- No GPU runs in CI. Every pure policy is tested there: the queue's cases, keys, eviction choices
  and the importers' lanes, by `node --test`. Metal and Dawn checks run on this Mac.

## Amendments

- **ADR 0006** (the helper): its options are flags in any order, `--level --definitions
  --workers N --structure`. The helper's descriptors are moved above 10 before they are placed,
  since a cache file at descriptor 3 was overwritten by the source.
- **ADR 0009** (composite): dual-source blending for a clear sheet; see decision 1.
- **ADR 0011** (cache): eviction by suffix and key group; partial-file sweeps; the gallery's
  caches beside the import cache.
- **ADR 0014 / 0015** (WebGPU, web runtime): alpha, `renderOffscreenAsync`, the thumbnail worker
  module, the import queue, OPFS v2.
