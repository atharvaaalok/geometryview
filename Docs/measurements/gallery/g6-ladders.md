# G6: Ladders

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window, with `--gallery-views all`;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900, with `?views=all`.

The folders are test-models (1,862 files) and designlib's output (166 files, read only). Both checks
now run Plates, Sheet and Ladders in Illustration, Blueprint and Studio, in a folder and its first
three subfolders, then G3's checks (sticky headings, search, Peek, the moves).

## Alignment, as painted

| Folder | Front end | Runs, all views | Ladders' runs | Groups over 0.5 pt | Furthest from the layout, Ladders |
|---|---|---|---|---|---|
| designlib's output | Mac | 27 | 9 | 0 | 0 pt |
| designlib's output | Chrome | 27 | 9 | 0 | 0.025 pt |
| test-models | Mac | 36 | 12 | 0 | 0 pt |
| test-models | Chrome | 36 | 12 | 0 | 0.028 pt |

In every view, the furthest any item was painted from the layout was 0 pt on the Mac. Chrome's was
0.208 pt: a detail bubble's turned leader in Plates, as in G4. There were 0 3D frames while the
gallery covered the view, and Chrome still scrolls test-models' largest folder (slices/hard) in 120
frames in 1.99 s with no long task.

## What the checks found and fixed

- Each stamp's box and its text shared an alignment group, and a box aligns on its bottom. The
  boxes now align on nothing; the stamps' texts share a baseline.
- Scales came out as fractions of a round number (1:1.25, 1:33.3), and a stamp's text, given a
  width rounded down, was cut. They now read 1:2.5, 1:40, 1:1000, whole.
- A ruler was measured at its rectangle's bottom, 7.25 pt from its line. Both painters now measure
  it on its line.

## Lanes

- A strip longer than its track shows how many more parts lie past it, and scrolls sideways by
  wheel, shifted wheel or drag. Its pictures are wanted first where the lane shows.
- Hits land where the parts show, not through the head column; a unit test scrolls a lane and hits
  a part past the track's first screen.
- Arrows along a strip scroll its lane to centre the selection.

## Tests

- `ctest --preset debug`: 324 doctest cases. Ladders' tests: alignment in three styles at both
  scales; lanes; calipers and the difference card; the stamps' scales; the lane reveal; no two
  texts on a line overlapping, at 374 to 1800 pt.
