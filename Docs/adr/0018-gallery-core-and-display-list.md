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
