# G0: the gallery's pictures

Measured on 2026-11-11 (early morning, Pacific Time) on the M4 Max (64 GB), with Chrome 157
(headless, WebGPU on Metal). The iPhone is measured with the thumbnail worker in G1.

## What G0 built

- **Folder colour.** `render::assignLooks` takes a first colour per document: that file's parts
  start there in the cycle (0 mustard, 1 mint, 2 tomato, 3 blush) and leave the shared count alone.
- **Line scale.** `render::Raster` carries the display scale, a line scale and a sheet alpha. The
  line scale thins outlines, creases, highlights, ghost lines, the wobble and soft shadows, but not
  grain, the grid or the composite's sampling.
- **A clear sheet.** With the sheet's alpha at 0 the frame comes out premultiplied: the part, its
  lines and its cast shadow. Metal's composite blends with two sources (ink, plus the colour beneath
  times what is kept of it), which leaves every opaque colour as it was. All 20 goldens pass
  unchanged. WGSL carries alpha the same way.
- **Render options.** `GeoRenderOptions` gains `pixels_per_point`, `line_scale`, `first_color`,
  `transparent` and `world_per_pixel`, and the defaults draw what they drew before.
- **`geo_style_sheet`** gives a style's sheet, for drawing it behind clear pictures.
- **The thumbnailer** (`geo_thumbnailer_*`, `Engine/engine/offscreen.mm`) keeps one renderer for
  many files. Each picture comes back in memory with the scale it was drawn at, the part's box from
  the frame's ids, and the box of everything drawn, its shadow included. `geo_render_blob_to_png`
  runs on it.
- **`gcli sheet`** draws many files on one sheet, as the gallery does.
- **`webgpu::Renderer::renderOffscreenAsync`** hands pictures back without waiting, for the browser.
  The bench page's `thumbs` mode times it.

## The Mac

`thumbnailer_test.mm`'s timing case, in the debug build, gives medians over 18 pictures of the
drive fixture after each style's first:

| Size | Kept thumbnailer | Fresh renderer per picture |
|---|---|---|
| 256 px | 2.5 ms | 12.1 ms |
| 512 px | 8.1 ms | 18.8 ms |

**Real files.** 1,000 files from test-models were each drawn at 256 px with the release `gcli sheet`,
in ten sheets of 100. The files' blobs are 0.10 MB at the median, 0.13 MB at p90 and 9.5 MB at most.
- Total: 3.9 s, about 3.9 ms a file.
- That covers opening each blob, drawing, compositing on the sheet, labels, and the PNGs.
- None failed.

The budget was 6 ms a picture, warm at 256 px; it is met.

## The browser

`Web/spike/renderer.html?thumbs=60&size=…` reads the cache file afresh for each picture, frames
it, and draws it on a clear sheet with lines at 0.5, one picture after another. Reports are in
`web/`.

| File | Size | Style | First picture | p50 | p90 | Wall per picture |
|---|---|---|---|---|---|---|
| demo drive (304 KB) | 256 | Illustration | 1219 ms | 2.36 ms | 2.96 ms | 22.8 ms |
| demo drive | 256 | Blueprint | 27 ms | 2.17 ms | 2.68 ms | 2.9 ms |
| demo drive | 256 | Studio | 30 ms | 2.31 ms | 2.79 ms | 3.2 ms |
| demo drive | 512 | Illustration | 31 ms | 2.73 ms | 3.17 ms | 3.5 ms |
| NEMA 17 (128 KB) | 128 | Clay | 43 ms | 2.27 ms | 2.65 ms | 3.2 ms |
| NEMA 17 | 256 | Metal | 131 ms | 2.07 ms | 2.52 ms | 4.7 ms |

- The first picture of the first run builds every pipeline, about 1.2 s. Later runs found Chrome's
  shader cache warm.
- After that, a picture takes 2–3 ms from reading the file to having its pixels.
- Wall time per picture includes the page's 20 ms polling.

## What this decides

- One kept renderer per process is the thumbnailer on both platforms. Building one per picture is
  5× slower on the Mac.
- The browser can draw pictures at the Mac's rate. Whether it needs its own worker and device is
  decided by the iPhone's memory in G1, not by speed.
- Small pictures need the line scale. At 48 pt with lines unscaled, a hex nut is mostly outline;
  at 0.5 it reads as a nut. The hosted viewer's samples show the same: unscaled, the flange's bolt
  holes and the pack's cells fill in (`line-weight-48pt.png`; the five samples at 48, 96 and
  160 pt in `illustration-48-96-160pt.png`, both from `gcli sheet`, drawn by 0.3.0's renderer).
