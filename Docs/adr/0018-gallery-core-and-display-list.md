# ADR 0018: The gallery's layout core and its display list

Status: accepted (2026-11-11). G2 of the gallery plan.

## Context

The gallery has to look the same on the Mac (AppKit) and in the browser (the DOM, also in the
iPhone shell). The approved design is strict about alignment:
- like items share every internal line;
- text aligns by baseline;
- labels hang from their row;
- each view has one left edge;
- the rules are checked by measurement, half a point at most.

Two front ends laying out on their own would drift apart, and every rule would be written twice.

## Decisions

**1. One layout core in C++, portable.**
- `Engine/gallery/` lays a folder out once, as a display list, in points: rectangles, lines, text
  and pictures, every colour resolved for the drawing style.
- It also holds the folder tree, the naming rules, the folder in view, hover, focus and selection.
  It answers hits and keys, and lists the files wanted, those in view first, in the library's
  bands.
- The Mac reaches it through `gallery.h`. The page reaches it through the engine's own wasm
  module (`web_gallery.cpp`), reading items straight from the module's memory.

**2. Text is set without a font engine.**
- IBM Plex Mono has one advance for every glyph (600/1000 em), so the core computes each line's
  width, cuts it with an ellipsis, and places its baseline. It reckons with the font's ascent
  (1025) and descent (275).
- On the Mac, Core Text reports the same ascent (1.02499 em, measured by `--gallery-check`).
- Browsers round a font's ascent to the pixel at some sizes. The DOM painter measures each size's
  baseline offset once, with a mark on the baseline, and places text by it.

**3. Painters only paint.**
- `GalleryView.swift` paints with reused Core Animation layers; `gallery-view.js` with reused,
  absolutely placed elements.
- Neither decides a position: both paint the items within a screen of the view, and scrolling
  moves the content.

**4. Alignment is data, and checked as painted.**
- Every row of like items carries a group, and every column a column group, with the line each
  member stands on: a baseline, a part's bottom, a rule, a left edge.
- `geo_gallery_check` takes where a painter actually drew each item and reports any group spread
  over half a point.
- Both front ends report it: `--gallery-check` on the Mac, `?test=gallery` in the browser. Each
  also reports how far anything was painted from where the core laid it.
- The Mac and the browser agree when both are within a few hundredths of a point of the same
  layout.

**5. Plates is the first view.**
- Subfolders are plates, with the columns that draw them largest. Each plate has a cover of up to
  nine parts at one scale on one to three baselines, an eyebrow of what is inside, a rhythm
  shared by every plate in the grid, and crop marks at rest.
- A folder's own files stand smallest to largest at one scale. Far larger files go on a row of
  their own, and a big folder falls into size bands, cut short until shown whole.
- Files not measured yet are listed, files that could not be read keep their place, and a folder
  with nothing to draw says so.

**6. The 3D view stops while covered.**
- `geo_engine_set_covered` (and `geo_web_set_covered`) stop the frame loop while the gallery
  covers the view; uncovered, it draws one frame. Both checks count 3D frames while covered:
  zero.

## Consequences

- The design's alignment rules are tested where they are made: the core's unit tests check every
  group on synthetic libraries, in three styles.
- Mac–browser parity holds by construction for the same facts and pictures. The pictures
  themselves come from Metal and from WebGPU, whose parts' boxes can differ by a pixel; layouts
  then differ by as much.
- A new view is a new layout function in the core, painted unchanged by both front ends.
- The core is small, pure and portable: it runs in the wasm unit tests too.

## Addendum (2026-11-11): G3, Sheet and the moves

**7. Sheet is the second layout, in the same core.**
- `layoutSheet` draws every file once, folder by folder, as the design's contact sheet. It shares
  its drawing of parts, scale bars and alignment with Plates (`Engine/gallery/draw.h`).
- A heading that stays at the top while its block scrolls under it is data: each of its items
  carries `sticky_top` and `sticky_until`, and a painter moves it down by how far the view has
  scrolled past, at most that far. Painters measure it less that offset, so the check still holds
  it to the layout.

**8. The gallery's state is the core's.**
- The core holds the views enabled, the view each folder last showed, Sheet's controls, the search
  and Peek. The front ends keep only Sheet's controls between launches.
- Esc steps back through Peek, the search, the selection and the folder, in the core. Typing
  searches and Backspace edits the search, so no key that edits text goes up a folder.

**9. Items can be fixed in the view.**
- Peek is items with the `Fixed` flag, in the view's points. Painters paint them in a layer over the
  content. While Peek shows, only its own targets can be hit.
- A rectangle with the `Sheet` flag is filled with the drawing style's own sheet, which each
  painter already draws for the gallery's ground.

**10. Moves are data; painters play them.**
- After a layout, `geo_gallery_transition` says what it plays against the one before:
  - a switch of views, where parts in both fly, new ones fade in from 35%, and the old view
    fades;
  - into a folder, which grows out of its plate;
  - back up, which shrinks into it.
- `geo_gallery_tracks` gives each part's start and timing. The ease is (0.3, 0.7, 0.2, 1).
- The Mac plays them with Core Animation, the browser with Web Animations. Each keeps the old view
  beneath as a copy: the Mac hands its old layers over, the browser clones its elements. Under
  Reduce Motion neither plays.
- Both checks read back the animations started and compare them with the spec's timings
  (`Docs/measurements/gallery/g3-sheet.md`).

**11. Painting order is the list's order.** Layers reused from one paint to the next are put back
in the display list's order among their siblings, not only given a z position. Whatever draws the
tree, a snapshot included, shows the same covering.

## Addendum (2026-11-11): G4, the states, the poster and the phone

**12. The states every view shares are drawn by the core's part drawing.** Both views get them:
- Reading: a mustard bar on the measured box, filling as the file is read.
- A file changed: a Δ until seen, its picture at 45% (`Stale`) until the new one comes.
- Tiny beside huge: a detail bubble on a shelf at one scale.

  Nothing else moves for any of them. The Mac watches a gallery's root with FSEvents and scans it
  again after a burst; a file whose time changed re-inks alone. The browser has no such events and
  re-reads a folder when it is shown again.

**13. A folder that can no longer be read is a state, not an error.** In the browser a connected
folder's permission can lapse. The page keeps each root's last listing, with the size and time
learned for each file drawn. A lapsed folder shows that listing faded (`Lost`) under a card with
Reconnect, which the core hits over the content. Its pictures are found among those kept by size
and time, without reading anything.

**14. The poster is a layout, not a screenshot.**
- `geo_gallery_set_poster` prints the view on one sheet: a head with the title, a foot with the
  path, the style and the date the front end passes. Plates shows every folder holding files as a
  plate; Sheet shows smaller cells and keeps no heading at the top.
- The Mac renders its own layers to a PNG, every item painted, not only those near the view. The
  browser draws the display list on a canvas, as its elements paint it. Both are checked for
  alignment like any view.

**15. The phone is a width, not a separate layout.**
- Below 480 pt Plates stands two abreast; Sheet keeps three columns; Peek puts its stats in two
  rows.
- A section's heading drops "ONE SCALE", then its count, before letting texts overlap. A width that
  ran to zero had meant "no limit"; a test now holds every line's texts apart from 374 to 1800 pt.
- The browser's checks run at an emulated 390 × 844 screen through DevTools, since Chrome makes no
  narrower window.

## Addendum (2026-11-11): G6, Ladders and lanes

**16. A lane is a band of the content that scrolls sideways on its own.**
- Ladders' strips can run to thousands of points, so each strip's parts sit in a lane. Its items
  carry the lane's id (a new item field; items are 116 bytes); the layout lists each lane's rect and
  content width.
- A painter clips each lane and moves its items by the lane's scroll: the Mac with a layer whose
  bounds' origin is the scroll, the browser with a clipped box holding a moved one. It paints a
  lane's width either side of what shows.
- The core is told each lane's scroll (`geo_gallery_set_lane_scroll`). Hits within the lane, the
  pictures wanted first, and the comparison card's place take it into account. After a key,
  `geo_gallery_reveal_lane` scrolls the selection's lane to it.

**17. A ruler is one item.**
- A strip's ruler is a rectangle flagged `Ruler`: its line, minor ticks every `radius` points,
  major ones every `tracking`. Each painter draws the ticks itself, so a 30,000-point strip is one
  item, not thousands. Its numbers are ordinary text. Painters measure a ruler on its line.

**18. Ladders follows the board.**
- Scales snap to drafting ratios written as drawings write them (2:1, 1:2.5, 1:40, 1:1000), each
  strip its own or one shared.
- A strip's few far larger parts go after a divider at a scale of their own.
- Parts stand by their ink on the ruler; that line is their alignment group.
- Hover shows calipers; with a part chosen, a card gives the difference.

## Addendum (2026-11-11): G7, Atlas and its camera

**19. A view can be a map seen through a camera.**
- Atlas lays the folder out in map units: regions squarified by the square root of their counts,
  districts inside them. It draws only what shows, in the view's points, through a camera the core
  holds (`geo_gallery_atlas_camera`, `_zoom`, `_pan`, `_level`).
- Text keeps its size on screen; the level of detail follows the zoom.
- The content never scrolls. Painters turn wheels, pinches and drags into the camera's moves and
  draw again.
- The map card is fixed in the view, like Peek's items, and takes its own clicks.

## Addendum (2026-11-11): G8, Catalogue and its book

**20. A view can be a book, turned a spread at a time.**
- Catalogue sets the folder as a printed parts catalogue: an inside cover and a title page, the
  contents, a chapter for each subfolder, a plate and tables for each folder, and an index. The
  core paginates the whole book from the tree, and lays out only the spread in view, fitted to
  the view on the drawing style's own sheet.
- The content never scrolls; it turns. The core holds the spread
  (`geo_gallery_catalogue`, `_set_catalogue_spread`, `_catalogue_index`). ← and → turn it, and a
  new target kind, a page (`GeoTargetPage`), turns to the page it names: lines of the contents,
  rows of an opener, the thumb index, the mosaic's parts.
- Everything on a page stays inside its text, clear of the fold; only the thumb index hangs past
  the edge. A unit test holds every spread to that.

**21. A toolbar folds before it is cut.**
- Each view brings its own controls, and Catalogue's are the widest. Short of room, the bar first
  drops the view switch's keys (they stay in its tooltips), then Poster and the Look pill drop
  their titles. The Mac and the browser fold by the same rule, and both checks record any part of
  the bar that is still cut off.

## Addendum (2026-11-11): G9, posters for every view, and PDF

**22. Every view prints, and a print is laid out by the core.**
- A poster is a layout of its own, not a picture of the screen: Ladders wraps every strip on one
  plate at one scale, framed, with a drafting title block; Atlas fits its whole map in a frame
  beside a cartouche, laid out at a scale its district names fit; Catalogue's is its open spread.
  Plates and Sheet keep G4's head and foot. `posterHead` takes an origin, for a head in a frame.
- Each front end draws a poster as it paints the view. The Mac draws its own layers into a PDF
  context, so text and lines stay vector; the browser draws the display list on a canvas and keeps
  each page as a JPEG in a PDF of its own writing (`pdf.js`). A Catalogue's PDF is the whole book,
  each spread drawn once its pictures have come.
- A poster too large to draw is drawn smaller, not refused: 60 million pixels at most, 32,767 a
  side, and PDF pages of 14,400 points at most.
