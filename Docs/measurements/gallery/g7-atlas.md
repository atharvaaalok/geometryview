# G7: Atlas

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window, with `--gallery-views all`;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900, with `?views=all`.

The folders are test-models (1,862 files) and designlib's output (166 files, read only). Both checks
run Plates, Sheet, Ladders and Atlas in Illustration, Blueprint and Studio, in a folder and its
first three subfolders. Atlas is measured at each of its three levels: fitted (regions), at the
districts, and closest (every part named). Then G3's checks run.

## Alignment, as painted

| Folder | Front end | Runs, all views | Atlas's runs, three levels | Groups over 0.5 pt | Furthest from the layout, Atlas |
|---|---|---|---|---|---|
| designlib's output | Mac | 54 | 27 | 0 | 0.0001 pt |
| designlib's output | Chrome | 54 | 27 | 0 | 0.030 pt |
| test-models | Mac | 72 | 36 | 0 | 0.0001 pt |
| test-models | Chrome | 72 | 36 | 0 | 0.030 pt |

The Mac's ten-thousandths of a point are float rounding through the camera. In other views, the
furthest any item was painted from the layout was 0 pt on the Mac and 0.208 pt in Chrome (a detail
bubble's turned leader in Plates, as in G4). There were 0 3D frames while the gallery covered the
view. Chrome still scrolls test-models' largest folder in 120 frames in 1.99 s, with no long task.

## What the checks found and fixed

- Landmarks far out stood on one line but each recorded its own foot, which varied with its shadow:
  up to 40 pt apart. They now record the shared line.
- Closest in, every cell's name shared one group across the grid's rows: 499 pt apart. Each row has
  its own now.
- Far out, the Gallery region's landmarks were a 2 m rail beside specks, drawn over the region's
  name. Landmarks now leave out collections and the far larger parts, and stand no taller than
  150 pt.

## The camera

- The map is laid out in map units, 2,400 wide, and drawn through a camera into the view's points.
  Only what shows is drawn, with text at its size on screen. The content never scrolls; it pans.
- A unit test zooms about a point and finds it under the pointer still.
- The levels: under 1.55 × the fitted zoom, regions; under 3.3 ×, districts; then every part.
- A search shows at least the districts and drops pins on its finds.
- Pan: a plain wheel or a drag. Zoom: a modified wheel, a pinch, a double-click on open map, + and −,
  or the toolbar's level.

## Tests

- `ctest --preset debug`: 327 doctest cases. Atlas's tests:
  - the fitted map in three styles;
  - each level drawing more;
  - zooming about a point;
  - the map card's clicks;
  - a search's pins;
  - no two texts on a line overlapping.
