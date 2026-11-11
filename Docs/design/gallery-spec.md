# The gallery, one design on the Mac and the web

The gallery shows a folder of STEP and STL files, of any tree and any depth, in five views:
Plates, Sheet, Ladders, Atlas and Catalogue. Its jobs are finding and opening parts, and showing
a library off.

This is the approved design (board round 2) as the build follows it. Measurements are in points,
which are CSS pixels in the browser. Where the build plan decided otherwise, the decision is
marked **Build**, and it wins over the board.

The layout is computed once, in C++ (`Engine/gallery/`, ADR 0018), and painted by both front
ends: Core Animation layers on the Mac, the DOM in the browser. The checks measure every alignment
group below, in every view, folder and style, and fail on any spread over half a point. The Mac
and the web must agree within 1 point.

## Where it lives

| Part | Rule |
|---|---|
| Entry | Clicking a folder in the Library card shows its gallery in the main window. The card stays as the navigator, with the folder in view selected. Opening a file (⏎ or a double-click) goes to the viewer; Esc or the masthead's back pill returns to the same place. |
| Window | The view area runs from the Library card's right edge plus 22 to the right edge less 18, and from 96 to the bottom less 84. The toolbar card is centred, 22 above the bottom. **Build:** the app's own positions win: the Library card's top is at 72 (`MainWindow.swift`), and the masthead stays where the app has it. |
| Show it off | Every view has a poster state: the full window less 30 each side, no cards, the title and a footer rule printed on the sheet. It exports as PNG (G4) or PDF (G9). |
| Folders | Any tree, any depth. A folder shows its subfolders and its own files. A folder of files only, with no subfolders, groups by measured size: under 20, 20–100, 100–500, over 500 mm. |
| Names | The file name without its extension, with a zero-padded ordering prefix dropped. A name that is only an id falls back to the product name inside the file, or else `#id · unnamed`. Folder names are prettified: underscores become spaces, and ISO and DIN codes are spaced. Nothing is specific to one library. |

## The view switch

| Part | Rule |
|---|---|
| Control | A segmented pill at the left of the toolbar: Plates, Sheet, Ladders, Atlas, Catalogue, each an icon and its name. |
| Keys | **Build:** ⌘1–⌘5 on the Mac, as Finder's views. ⌥1–⌥5 in the browser, matched on `event.code`, since browsers keep ⌘1–⌘5 for tabs. In the gallery, typing digits searches; plain 1, 2 and 3 stay the viewer's face, body and part modes. |
| Releases | **Build:** 0.2.0 ships Plates and Sheet; the others are behind `--gallery-views all` on the Mac and `?views=all` in the browser until 0.3.0. |
| Memory | A gallery opens in Plates. Moving between folders keeps the view you are in, and a folder you come back to shows the view you last used there. |
| Shared state | The folder in view, search, selection, hover, drawing style and sort carry across views. The selected part is scrolled into view in the new one. |
| Toolbar | After the switch come only the view's own controls, then the Look pill naming the drawing style. Sheet has Scale, Size and Sort; Ladders, Sort and Scale; Atlas, zoom; Catalogue, pages, Contents, Index and Export. |

## Pictures

| Part | Rule |
|---|---|
| Camera | Orthographic, azimuth 35°, elevation 30°, the file framed alone (fit margin 1.12). The drawing style comes from the Look card; Illustration by default. |
| Colour | A file of several parts keeps its part colours. A file of one part takes its folder's colour, cycling mustard, mint, tomato and blush by folder, so folders read apart, in every style that colours parts (`first_color`). |
| Background | Clear, so a part stands on the window's own sheet in every style. The sheet is drawn from `geo_style_sheet`. |
| Sizes | Rendered at 2× the largest size shown (256 px for a 128 pt cell), with lines drawn for the size: `line_scale` = clamp(cell / 320, 0.5, 1). Layout uses the part's own box (`part_rect`); soft shadows may spill past it. |
| Cache | Keyed like the import cache (path, device, inode, size, time), plus style, size and the engine's version (ADR 0017). Tiles in view come first, and a changed file re-inks alone. |

## Scale

| Mode | Rule |
|---|---|
| Fit | Each part fills its place, at most 1.5× its render. |
| Folder | One scale per folder, so a folder's sizes compare honestly. Files more than 2–3× the folder's median size go on a "larger files · drawn n× smaller" row, or are marked SMALLER where cells are fixed. |
| True | One scale for everything in view. A part under 10 pt is drawn larger, with a DETAIL n:1 stamp (2, 5, 10, 20, 50 or 100), the drawing-office convention. |
| Scale bar | An ink bar of 1, 2 or 5 × 10ⁿ mm, 40–120 pt long, with its length above it, as in the viewer. |

## Alignment

| Rule | Applies to |
|---|---|
| Rows of like items share every internal line | In a row of plates, the eyebrow, picture band, base rule, title, meta and strip sit at one height, and the space is kept when a plate has no strip or eyebrow. Plate grids align by row and by column. |
| Text aligns by baseline, not by box | Headings with their counts, titles with meta and scale labels, a part's label with its neighbours', table cells in a row, names with counts in the Library card, toolbar labels with their controls. |
| Labels hang from the row, not the part | Under a shelf, every label sits one line below the row's baseline and deepest shadow, whatever each part's own shadow. |
| One left edge per view | Headings, folder rows and content start on one edge. Insets are 14 inside plates and cards, and mirror on facing pages. |
| Numbers | Tabular figures, right-aligned in columns. Dot leaders end at one edge, and page numbers align on it. |
| Checked by measurement | A check measures every group above in every view, folder, task and style, and fails on any spread over half a point. |

Text is IBM Plex Mono, so its layout is computed without a font engine: an advance of 600/1000
em, an ascent of 1025 and a descent of 275 per 1000.

## Motion

Ease (0.3, 0.7, 0.2, 1); all of it instant under Reduce Motion.

| Moment | Rule |
|---|---|
| Switching views | Parts in both views fly from their old place and size to the new, 520 ms. Parts new to the view fade in from 35% of the way. The old view fades out over 260 ms. |
| Into a folder (Plates) | The folder grows out of its plate, 420 ms; going up shrinks it back into the plate it came from. |
| Changing style | Pictures re-ink nearest the centre first: each fades to 15% over 90 ms, takes its new picture, and returns over 220 ms, staggered 6 ms apart, at most 0.9 s in all. The masthead's mustard pill counts "drawing n of N". |
| Opening a part | The viewer opens from the part's rectangle, 340 ms, as a clip that grows to the window. |
| Hover | A part lifts 2 pt up and left into a sticker on the style's lift colour, with a 3 pt ink shadow, 160 ms. |

## Colour on the sheet, per style

| Style | Text | Muted | Rule | Lift | Sheet |
|---|---|---|---|---|---|
| Illustration | #231E16 | #6F685C | #DDD6C8 | #F7F5EE | paper #F2EFE7 with grain |
| Metal | #231E16 | #6F685C | #DDD6C8 | #FFFFFF | #F6F5F3 |
| Clay | #231E16 | #6F685C | #DDD6C8 | #FAF8F4 | #F5F3EF → #E9E5DE, vignette |
| Blueprint | #EEF4FF | #A9BEDF | white 22% | #2A5592 | #1E4479 → #173765, 24 pt grid |
| Technical | #231E16 | #6F685C | #DDD6C8 | #FFFFFF | #FFFFFF |
| Studio | #ECEAE4 | #A8A59C | white 18% | #33353A | #17181B → #3A3C40, vignette |

## Keys

| Key | Does |
|---|---|
| ⌘1 … ⌘5 (⌥1 … ⌥5 in the browser) | Plates, Sheet, Ladders, Atlas, Catalogue |
| Arrows | Move the selection to the nearest part in that direction (Catalogue: ← → turn pages) |
| Space | Peek: the part large, with its measured size, parts, triangles and file; turnable in G9 |
| ⏎ · double-click | Open in the viewer · ⌘⏎ adds it to the view |
| ⌘↑ · ⌘↓ | Up a folder · into the selected folder |
| Esc | Closes Peek or a card, then clears the search, then the selection, then goes up a folder |
| Typing | Searches this folder: names, product names, folders. **Build:** Backspace only edits the search; it never goes up a folder |
| ⇧⌘L | The Look card: six styles, the same choice as the viewer |

## Views

| View | Rules |
|---|---|
| Plates | Subfolders as plates, with columns chosen to draw plates largest with the fewest empty cells. A cover is up to nine parts sampled across sizes at one scale, on one to three baselines; hover leafs through more. Crop marks at rest, a sticker on hover. A folder's files stand on a shelf at one scale, with a scale bar. |
| Sheet | Headings per subfolder stay at the top while scrolling, on the sheet's band colour. Cells of a chosen size (72–200 pt), parts standing on the cell's baseline, two-line labels. At most 240 cells per folder before "show all". |
| Ladders | One strip per folder, smallest to largest, on a ruler at the strip's scale (ticks of 1, 2 or 5 × 10ⁿ mm). Hover shows calipers with the measured box; choose one, then hover another for the difference. Each strip at its own scale, or one shared. |
| Atlas | Regions are subfolders, their areas growing with the square root of their file count, nested by the tree. Three levels of detail by zoom, a minimap card, and pins dropped by search. |
| Catalogue | Two-page spreads: a cover, contents (every part at once), chapter openers, a spread per folder (a true-scale plate and a table of measured sizes), and an index. A thumb index follows the tree. |

## States every view shares

| State | Rule |
|---|---|
| Reading | A file still being read shows its measured box (estimated from its structure) in dashed construction lines and a mustard bar. Its drawing then inks in over the same box, so nothing moves when it arrives. |
| Couldn't read | A file that fails keeps its place: a hatched box, its name and the reason, in ink. |
| Empty folder | A folder with nothing to draw says so, in the Library's words: "FOLDER · 0 FILES", "No STEP or STL files here." |
| No name in the file | When a file name is only an id, the name inside the file is used; when there is none, the id stays, marked unnamed. |
| A file changed | When a file in view changes on disk, only its picture re-inks, with a Δ until it has been seen. Nothing else moves. |
| Permission lost | In the browser, a folder whose permission has lapsed keeps its last pictures, faded, with one way back: Reconnect. |
| Tiny beside huge | At one scale, a part too small to see gets a detail bubble: a circle on the view and the part again, larger, at a stated scale (DETAIL A 20:1). |

## On the iPhone

At 393 × 852, Plates shows two columns of plates, and a folder's shelf fits the width. The Sheet
shows three columns. The view switch lives in the bottom sheet, raised from the toolbar as the
Library card is today. The page stays within 600 MB, and nothing is prefetched.

The board's prototype, with its real pictures, is the reference for anything this leaves out:
https://claude.ai/artifact/Sa2Q4M55r6Gs87oCJJBxoG (private).
