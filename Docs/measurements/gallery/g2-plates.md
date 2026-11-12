# G2: Plates, on the Mac and in the browser

Measured on 2026-11-11 (morning, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900.

The folders:
- test-models (1,862 files);
- designlib's output (166 files), read only.

## Alignment, as painted

Both checks measure every alignment group from what was actually painted:
- on the Mac (`--gallery-check`), from the layers' frames, with each text's baseline at its frame
  plus Core Text's ascent;
- in the browser (`?test=gallery`), from the elements' boxes, with each text's baseline from a
  mark the browser sets on it.

Each runs in Illustration, Blueprint and Studio, in the folder and its first three subfolders.

| Folder | Front end | Groups checked a run | Groups over 0.5 pt | Furthest from the layout |
|---|---|---|---|---|
| designlib's output | Mac | 4–26 | 0 | 0 pt |
| test-models | Mac | 4–16 | 0 | 0 pt |
| designlib's output | Chrome | 4–26 | 0 | 0.026 pt |
| test-models/models | Chrome | 4 | 0 | 0.022 pt |
| test-models (1,862 files) | Chrome | 4–15 | 0 | 0.025 pt |

- Core Text reports IBM Plex Mono's ascent as 1.02499 em and its descent as 0.27499 em. The
  layout core reckons with 1.025 and 0.275.
- Chrome rounds the ascent at some sizes. Before the DOM painter measured each size's baseline,
  a section heading's texts spread 0.55 pt and items sat up to 0.8 pt off. Measured, everything
  is within 0.026 pt.

## The Mac and the browser agree

Both front ends paint the same core's display list. The Mac paints it exactly (0 pt) and Chrome
within 0.026 pt, so for the same facts and pictures they agree within 0.03 pt, well inside the
1 pt asked.

Their pictures do differ:
- each front end draws its own, with Metal or with WebGPU;
- a part's box can differ by a pixel between the two, 0.6 pt at most at the largest scale shown;
- layouts then differ by as much.

## No 3D frames while the gallery covers the view

| Front end | Frames in 3 s (Mac) or 2 s (Chrome), the gallery showing |
|---|---|
| Mac | 0 |
| Chrome | 0 |

## Scrolling in the browser

`?test=gallery&scroll=slices/hard&expand=1&scrollOnly=1`
(`web/mac-chrome-g2-gallery-scroll-slices-hard.json`) shows test-models' largest folder,
slices/hard, with every size band shown whole: 614 files, 1,696 items, 7,048 pt of content. It
scrolls top to bottom in 120 frames.

| Frames | Time | Long tasks over 50 ms |
|---|---|---|
| 120 | 2.0 s (60 a second) | none |

The painter repaints a window of three screens as the view nears its edge, with reused elements.

## The library behind it

On test-models, the Mac's check runs the library service as the gallery does:
- 765 files imported, 1,695 pictures drawn, 223 from the cache, 0 failures;
- the plates in view asked for a sample of their files first, then the rest prefetched.

The folder's scan took 0.11 s.
