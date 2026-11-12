# G4: the states, the poster, the phone and view_folder

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900, and at an emulated phone screen of
  390 × 844 at 3 pixels a point.

The folders are test-models (1,862 files) and designlib's output (166 files, read only).

## G2's and G3's criteria still hold

Every check of G2 and G3 ran again on the finished G4 code, on both folders: alignment in Plates
and Sheet, in three styles. G3's checks ran as well: the sticky headings, the search, Peek, and
the moves read back.

| Folder | Front end | Runs | Groups over 0.5 pt | Furthest from the layout | 3D frames covered |
|---|---|---|---|---|---|
| designlib's output | Mac | 18 | 0 | 0 pt | 0 |
| designlib's output | Chrome | 18 | 0 | 0.026 pt | 0 |
| designlib's output | Chrome as a phone, 390 × 844 | 18 | 0 | 0.027 pt | 0 |
| test-models | Mac | 24 | 0 | 0 pt, once a leader's line was fixed (below) | 0 |
| test-models | Chrome | 24 | 0 | 0.208 pt, a detail's turned leader | 0 |

In Chrome, test-models' largest folder (slices/hard, 1,696 items) still scrolls in 120 frames in
1.99 s, with no long task.

On both front ends the moves played at the spec's timings and the selection was kept.
- The switch: 520 ms flights, fade-ins at 182 + 338 ms, 160 and 260 ms fades.
- The zoom: 420 ms.

### A detail's leader, found by these runs

The test-models runs measured an item 41.8 pt from the layout on both front ends. It was the new
detail bubble's dashed leader, in test-models/models: a 1 mm triangle stands on the shelf of a
346 mm battery pack. A line's recorded line was its start, and painters measure its top. The
browser also drew the leader straight up: its DOM painter knew only level and upright lines.
Both were fixed (`5bd4870`). Checked again on test-models/models:
- the Mac within 0 pt of the layout;
- Chrome within 0.197 pt, the turned line's box;
- 0 groups over.

## A file changed re-inks alone (Mac)

`--gallery-watch test-models/models/gp52c_flange.step`, the folder in Sheet, the file touched
from a shell once the app said it was ready (its times restored after):

| Δ shown | Picture faded while drawn again | New picture | Imports | Other items moved |
|---|---|---|---|---|
| 1.56 s after the touch | yes | 1.67 s | 1, the file's | 0 of 58 |

The Δ stays until the file is seen: hovered, selected, peeked or opened.

## The poster

| Front end | View | Groups over 0.5 pt | Furthest from the layout | PNG |
|---|---|---|---|---|
| Mac (`--gallery-poster`) | Plates | 0 | 0 pt | 2880 × 1843 px |
| Mac | Sheet | 0 | 0 pt | 2880 × 6182 px |
| Chrome (`?poster=1`) | Plates | 0 | 0.026 pt | canvas, 960 KB |
| Chrome | Sheet | 0 | 0.025 pt | canvas, 3.6 MB |

Designlib's output in Plates prints 11 folders as plates on one page, under its title, with a
footer rule.

## The phone, in Chrome

At an emulated 390 × 844 screen (`?phone=1`):
- **No sideways scroll.** The page and the gallery both have 0 pt of overflow.
- **Plates.** Folders stand two abreast.
- **Sheet.** Three columns.
- **Peek.** It fits, with its stats in two rows.
- **Toolbar.** Icons only: the Library, the views and the Look.

## A folder whose permission lapsed (Chrome)

`?lost=1` draws designlib's output, makes the folder unreadable, and shows it again.

| Pictures back from those kept | Items faded | Groups over 0.5 pt | Readable again |
|---|---|---|---|
| 41 of 41 | 87, under the card with Reconnect | 0 | none faded |

## view_folder (MCP)

test-models/models, 9 files, from the cache: one contact sheet and its index in 2 s. The MCP tests
run it through the tool server: a folder pictured, a filter matching nothing, and a file passed as a
folder.

## Not yet measured: the iPhone's memory

G4 asks the iPhone to stay within 600 MB on test-models. That needs:
- test-models on the phone, connected in the shell through the Files picker, which takes a person;
- the web process's memory, which only Instruments can read. It lost the phone over Wi-Fi in G1.

It is left for when the phone can be cabled and the folder connected.

## Tests

- `ctest --preset debug`: 319 doctest cases. G4's tests:
  - reading and changed marks that move nothing;
  - the poster in both views;
  - the phone's widths;
  - no two texts on one line overlapping, from 374 to 1800 pt;
  - the lost state's card and hits;
  - the detail bubble.
- The same core in wasm: 236 cases.
- `npm test` in Launcher: 60 of 61, 1 skipped as before. Among them are the five tools,
  view_folder, and the cache's sweep.

## The iPhone's memory (measured 2026-11-11, evening, Pacific Time)

The gallery's check on test-models (1,862 files, 4.4 GB, copied into the shell's Documents), run
by the shell on an iPhone 16 Pro over USB with all five views in Illustration, while Instruments'
Activity Monitor recorded every process:

| Run | The page's peak | Median | What happened |
|---|---|---|---|
| Before | 916 MB, sustained 600-780 MB | — | a memory warning, then the page was killed after about a minute |
| After | **489 MB** | 294 MB | the whole check: 44 runs, 0 groups over 0.5 pt, 120 frames scrolled in 2.0 s |

The criterion was at most 600 MB, with a gate of 900 MB: met. What it took, on phones only:
- **Files fetched to be looked up.** The shell's store reads a file whole, and looking up a picture
  fetched its file first for its size and time, so a 500 MB STL passed through the page even when
  its picture was kept. On the shell, the walk's sizes and times find a file in the caches now.
- **Imports.** STEP files over 12 MB and STL files over 16 MB are left to a computer, shown as
  files that could not be drawn, saying why; the importer is let go after each file; no detail
  pictures.
- **Pictures held decoded.** 320-pixel pictures, not supersampled; half a view painted beyond
  what shows; a picture no longer painted lets go of its image; the picture worker replaced above
  128 MB.

Instruments lost the phone about a minute into every recording over Wi-Fi; over USB it recorded
throughout. Report: `iphone/iphone-loopback-g4mem5-g9-gallery-test-models.json`.
