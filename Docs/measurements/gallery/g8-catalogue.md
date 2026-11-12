# G8: Catalogue

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max:
- the Mac app's debug build, in a 1440 × 900 window, with `--gallery-views all`;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900, with `?views=all`.

The folders are test-models (1,862 files) and designlib's output (166 files, read only). Both checks
run all five views in Illustration, Blueprint and Studio, in a folder and its first three
subfolders. Catalogue is measured at its cover and at up to five more spreads: the contents, the
next two, the middle and the last (the index's end). Then G3's checks run.

## Alignment, as painted

| Folder | Front end | Runs, all views | Catalogue's runs | Groups over 0.5 pt | Furthest from the layout, Catalogue |
|---|---|---|---|---|---|
| designlib's output | Mac | 102 | 48 | 0 | 0.0000 pt |
| designlib's output | Chrome | 102 | 48 | 0 | 0.028 pt |
| test-models | Mac | 132 | 60 | 0 | 0.0000 pt |
| test-models | Chrome | 132 | 60 | 0 | 0.030 pt |

In other views, the furthest any item was painted from the layout was 0.0001 pt on the Mac (Atlas's
camera) and 0.208 pt in Chrome (a detail bubble's turned leader in Plates, as in G4). There were 0 3D
frames while the gallery covered the view. Chrome still scrolls test-models' largest folder in 120
frames in 1.99 s, with no long task, and nothing runs sideways off the page at 1440.

The toolbar was never cut off on either front end, in any run. It folded the same way on both:
Sheet's controls fold Poster's and Look's titles away at 1440, Catalogue's the view switch's keys,
and the other views fit whole.

## What the checks found and fixed

- **Plates ran off the page.** A plate's rows were filled greedily and the last took what was left:
  designlib's washers ran past the page's edge and across the fold. The plate is now the board's:
  up to 13 parts across their sizes in one row, packed by their ink (shadows too) and drawn smaller
  until the row fits; a caption says how many and how many larger files were left out; a figure
  shows the typical part, or the row pointed at, as large as its picture stays sharp.
- **The tables cut their last column** to "TRIANG…". They now have the board's columns: a small
  picture, the file, its size, parts, triangles in thousands ("Tris") and the bytes on disk. Counts
  of parts from 10,000 are in thousands too, so rack1m's million fits.
- **The frontispiece** took each folder's middle file, collections and all: 2 m rails beside specks.
  It now takes each folder's typical part (the usual number of parts, a real name, the middle size),
  keeps those within 8 times the middle one's size, says how many it left out, and has a scale bar.
- **Edges.** The glance page's axis labels hung past the text at both ends; the contents' mosaic
  fitted each part's box but not its shadow.
- **The toolbar** cut off the Look pill in Catalogue at 1440, in Chrome; on the Mac it had no limit
  at all, and Sheet's already ran past its room. Both now fold before they are cut (ADR 0018, 21).
  In Chrome the bar was also narrower than what it held: it is sized from its contents, and the
  pages label could wrap in that measure. It is kept on one line.
- **A box no deeper than its strokes** grew in Chrome: a bar of the glance page's histogram, 1.3 pt
  tall with a 1 pt stroke, became 2 pt, since Chrome rounds a border up to a whole pixel. It was
  0.5 to 0.7 pt off its line, in whichever style happened to catch the histogram that low. A Mac
  layer draws such a box as all stroke and keeps its frame; the browser now fills it with the
  stroke colour. The check now names the item furthest off in any run off by more than 0.1 pt,
  which is how this one was found.

## The book

- The core paginates the whole book from the tree and lays out only the spread in view, fitted to
  the view on the drawing style's sheet. It turns; it never scrolls.
- designlib's output is a book of 35 pages; test-models' 115 on the Mac and 113 in Chrome (below).
- A unit test holds every spread to its page: nothing but the thumb index leaves the text or crosses
  the fold, in the test folder and in a folder of washers like designlib's. With the plate's fix
  undone, it fails.

## Open

- **Names read inside files.** The Mac reads the product names inside test-models' STEP files
  ("SHELL", "Spur-3", "upper housing"); the browser's facts lack many of them. So in Chrome more of
  Sweep's files are known only by number (Random 242 and Stress 224, against 1 on the Mac), the
  index is shorter and the book has a spread fewer. The layouts agree; what the importers read
  differs. It shows in every view's names, and is for the import side, not the gallery.

## Tests

- `ctest --preset debug`: 2 of 2 (330 doctest cases); wasm: 247 cases. Catalogue's tests:
  - the book in three styles, every spread aligned, every file in its tables once;
  - everything on a page staying on it, clear of the fold;
  - the arrows turning the book, and a contents line turning to its page.
- Reports: `web/mac-chrome-g8-gallery-{designlib,test-models}.json`.
