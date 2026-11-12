# G3: Sheet, the view switch, Peek and the moves

Measured on 2026-11-11 (late morning, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900.

The folders:
- test-models (1,862 files);
- designlib's output (166 files), read only.

Both checks now run every view enabled (Plates and Sheet) in Illustration, Blueprint and Studio,
in the folder and its first three subfolders. Then they check the sticky headings, the search,
Peek and the moves: `--gallery-check` on the Mac, `?test=gallery` in the browser
(`web/mac-chrome-g3-gallery-{designlib,test-models}.json`).

## Alignment, as painted

| Folder | Front end | Runs | Groups checked a run | Groups over 0.5 pt | Furthest from the layout |
|---|---|---|---|---|---|
| designlib's output | Mac | 18 | 4–39 | 0 | 0 pt |
| designlib's output | Chrome | 18 | 4–33 | 0 | 0.026 pt |
| test-models | Mac | 24 | 4–46 | 0 | 0 pt |
| test-models | Chrome | 24 | 4–38 | 0 | 0.029 pt |

G2's criteria still hold in Plates, and hold in Sheet too:
- the Mac and Chrome agree within 0.03 pt for the same pictures;
- 0 3D frames while the gallery covers the view, on both.

## Sheet's headings stay at the top

Scrolled a third of the way down the top folder in Sheet, the heading of the block in view sits at
the view's top:

| Folder | Mac | Chrome |
|---|---|---|
| designlib's output | 0 pt | −0.011 pt |
| test-models | 0 pt | −0.006 pt |

Alignment, measured with each heading less the offset it was moved by, stays at 0 groups over.

The Mac's first run measured −199 pt. A reused layer kept a heading's old offset, and a frame set
through it landed off by as much. Layers are now placed afresh. The check measures the layer as
drawn, its transform included.

## The moves

The core sets each move's timing (`geo_gallery_transition`, `geo_gallery_tracks`). The checks
switch from Plates to Sheet, which shows parts Plates does not, so both kinds of track play. Then
they go into a folder from its plate and back up. Each check reads back the animations its front
end actually started:

| Move | Spec | Mac (Core Animation) | Chrome (Web Animations) |
|---|---|---|---|
| Switch: parts in both views fly | 520 ms | transform 0+520 | 0+520 |
| Switch: parts new to the view fade in | from 35% (182 + 338 ms) | opacity 181+338, read 1 ms after it began | 182+338 |
| Switch: the new view comes in | 160 ms | opacity 0+160 | 0+160 |
| Switch: the old view fades out | 260 ms | opacity 0+260 | 0+260 |
| Into a folder: it grows out of its plate | 420 ms | transform and opacity 0+420 | 0+420 |
| Back up: it shrinks into its plate | 420 ms | the core's transition: 420 ms, into the plate it came from | the same |

- The selection was kept across every switch checked, on both front ends.
- Fade-ins were measured on designlib's output. On test-models, the switch checked was in a
  folder where Plates already shows every part, so all of them flew.
- Into and out of a folder, the plate's rectangle is the same both ways: 521 pt wide at (6, 2) in
  both folders. Its height follows the view's height, so it differs between the Mac window and
  Chrome's.
- The animations were read back on the way in. For the way back up the checks read the core's
  transition only.
- Under Reduce Motion neither front end plays anything. That is in the code; the checks do not
  turn Reduce Motion on.

## Shadows in Sheet

The first screenshots showed a deep shadow (designlib's `stepper_17hs19.zones`) cast over its own
label. Sheet now fits each part with its shadow, and stands a row's parts on one line raised by
the row's deepest shadow (`aaeb115`). The numbers here are from after that change.

## Search and Peek

| | designlib's output | test-models |
|---|---|---|
| A search of a name's first three letters | 69 found on the Mac ("bea"), 29 in Chrome ("600") | 3 ("rac"), 221 ("668") |
| Items drawn at 22%, not found | 24 layers, 48 elements | 267 layers, 207 elements |
| Alignment while searching | 0 groups over | 0 groups over |
| Peek's items, fixed over the view | 20 layers, 19 elements | 20 layers, 19 elements |
| Peek's alignment (its stats on two baselines) | 0 groups over | 0 groups over |

The Mac draws the card's shadow as a layer of its own; the browser draws it as a box shadow. The
two front ends searched different words because each takes the first file it wants.

## Scrolling in the browser

`?test=gallery&folder=test-models&scroll=slices/hard&expand=1` shows test-models' largest folder,
slices/hard, with every size band shown whole: 614 files, 1,696 items, 7,195 pt. It scrolls top to
bottom in 120 frames:

| Frames | Time | Long tasks over 50 ms |
|---|---|---|
| 120 | 2.0 s (60 a second) | none |

## Tests

- `ctest --preset debug`: 313 doctest cases. Among them:
  - Sheet's alignment in three styles and three scales, its headings and size bands, and every
    part's shadow kept above its labels;
  - the views and each folder's memory of its view;
  - the moves' timings;
  - the search, Peek's clicks and arrows, and the Esc chain.
- The same core in wasm: 230 cases.
- `npm test` in Launcher: 58 of 59, 1 skipped as before, with the five view icons in icons.js.
