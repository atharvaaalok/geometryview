# The chrome, one design on the Mac and the web

The Mac app (AppKit, `App/Sources`) and the web viewer (`Web/viewer`, also in the iPhone shell)
draw the same chrome. This lists what both must agree on. Where the two differed, the Mac's version
was kept, except where the Mac used a stock system control: there the page's own control became the
design, and the Mac draws it too (the search field, the menus, the Library rows).

`Tools/compare_chrome.py` checks the layout: the Mac's `--chrome-layout` report and
`Tools/web_chrome.mjs`'s, for the same file at the same window size, must agree within 1 point.

```sh
open -n -a build/release/App/GeometryView.app part.step --args --chrome-layout
node Web/spike/serve.mjs 7342 &   # serves the viewer and build/fixtures
node Tools/web_chrome.mjs "http://127.0.0.1:7342/viewer/?import=part.step" /tmp/web
python3 Tools/compare_chrome.py ~/Library/Containers/com.aalok.geometryview/Data/tmp/chrome-layout.json /tmp/web.json
```

## Conventions

- **Points and CSS pixels.** A Mac point is a CSS pixel. A card's 1.5 line is inside its box on both,
  so a web padding is the Mac's inset less 1.5.
- **Line.** 1.5 everywhere, snapped to whole device pixels on the Mac. Swatches, row buttons and key
  caps use 1.25.
- **Motion.** 0.16 s for state changes, 0.22 s for the progress bar, curve (0.3, 0.7, 0.2, 1). None
  when the system asks for reduced motion.
- **Icons.** One set: `Web/viewer/icons.js`, 16-unit line drawings stroked 1.5 units in the text
  colour. The Mac reads the same file from its bundle (`Icons.swift`). Sizes: pills 15, row buttons
  13, tree marks and chevrons 12, the file glyph 14, the assembly glyph 16.

## Tokens

| Name | Value | Name | Value |
|---|---|---|---|
| paper | `#F2EFE7` | cream | `#FFF4D6` |
| sheet | `#F7F5EE` | cream wash | `#FBF5E2` |
| sheet sunk | `#ECE8DD` | cobalt mid | `#5A82C6` |
| ink | `#231E16` | cobalt wash | `#DDE6F4` |
| ink soft | `#3A342A` | mustard | `#F4B829` |
| muted | `#6F685C` | tomato wash | `#F8DDD3` |
| faint | `#9A9284` | ink on dark | `#EEF4FF` |
| rule | `#DDD6C8` | muted on dark | `#A9BEDF` |

Type is IBM Plex Mono: 10.5 (card titles, paths, match counts), 11.5 (buttons, segments, counts,
labels), 12.5 (tree names, search, status text), 13.5 (masthead title, selection name), 15 (status
heading), 26 (selection numbers). Weights 400, 500 and 600.

Radii: cards 12, chips and menus 8, rows and row buttons 6, swatches 3 (bodies 2), key caps 5.
Shadows are solid ink, unblurred: cards 3, chips and menus 2, key caps 1.

## Parts

| Part | Both draw |
|---|---|
| Masthead | Title 13.5/600, 2 above the subtitle 11.5 muted, each with a paper halo of 3. Progress, only while importing: a 240×6 pill, sheet with an ink line and r3, filled with mustard up to a 1.5 ink edge, 10 below the subtitle. The Mac's sits beside its window buttons; the page's 24 in, 10 down. |
| Structure card | 300 wide, 18 from the left, from 72 to 96 above the bottom. Switcher 16 in and 12 down; search 10 below it, 16 in; the tree 10 below that, 6 in from the card, 8 above its bottom. |
| Switcher | 30 tall, r15, sheet, an ink outline over its segments. Segments as wide as their labels plus 11 each side, divided by 1.5 ink; 11.5 at 500, the chosen one at 600 on cream with a 2 ink underline above the outline. Hover cream wash. |
| Search | 28 tall, r14, white, 1.5 rule line, ink while focused. Magnifier 14 muted, 10 inside the line, then 6, the text 12.5 (placeholder muted), and the match count 10.5 muted at the end. Return steps through matches, Escape clears. |
| Tree rows | 28 tall (30 on phones). Chevron in a 16 box at 8 + 14 a level; 3 later a 16 slot for the glyph or swatch; 7 later the name 12.5; the count 11.5 muted ends 14 from the row's edge. Behind, a plate inset 6 and 1, r6: cream under the pointer or the pointer's part in the view, and when selected, cobalt wash with a 3 cobalt bar down its left edge, clipped to the plate. Hidden names faint and struck through, ghosted muted; at the end an eye-slash, ghost or half-filled mark, 12 muted. |
| Swatch | Parts 12, bodies 9, 1.25 ink line. The colour over the top left; past a line across the diagonal 55% of the way, the colour 16% toward ink. |
| Row buttons | Ghost, hide and isolate, 22×22, 1 apart, 8 from the edge, under the pointer: bare, then sheet with an ink line on hover, sheet sunk with an ink line when on. |
| Pills | 30 tall, r15, sheet, ink line, 11.5/500, 12 at the sides, icon and text 6 apart. Hover cream; pressed, 1 right and 1 down; on, ink with paper text; disabled at 0.4. |
| Status card | 360 wide, centred, 20 inside. Heading 15/600, 8 below it the text 12.5 muted. |
| Toolbar | A card 22 above the bottom, centred right of the structure card; 10 at the sides, 8 above and below, items 8 apart, dividers 1.5×22 rule. Its room is the window's width less 336 (less 16 on phones, below 900 and with the sidebar away). When the tools don't fit, the least used fold into a More button at the end, a rank at a time: "?", the projection, the views and the picture, fit, the add pair, look, section, see-through, isolate, ghost and hide, undo and redo, show all (`Web/viewer/toolbar-fold.js`, and `Toolbar.foldPlan` on the Mac). A divider goes with its group. Worked out when the width changes, never per frame. |
| More menu | The folded tools in toolbar order, each a row with its icon and name; the projection as Orthographic and Perspective, the current one checked; the views under a "Views" heading; the picture as Copy picture and Save picture. Rows that can't be used now at 0.4. |
| Menus | A card with r8 and a 2 shadow, 6 inside; rows 11.5 with 6 above and below and 12 at the sides, r6, cream under the pointer. Opens 10 above its button, kept 8 inside the window; closes on a choice, Escape or a click elsewhere. |
| Selection card | 286 wide, 18 from the right, 172 down; 14 at the sides, 12 above, 14 below, sections 12 apart. |
| Look and section cards | Centred over the toolbar, 16 at the sides, 14 above, 16 below, rows 10 apart; labels 92 wide, 11.5/500; rows that don't apply dimmed (pills 0.4, labels faint). Section card 340 wide. |
| Drop zone | 14 from the window's edges, cream at 70%, r18, a 2 ink line dashed 8 and 6; "Drop to open. Hold ⌘ to add to what is shown." 13.5/600 in the middle. |
| Hold hint | A chip 86 above the bottom: tomato wash for H, sheet for G, a key cap and the words. |
| Shortcuts | A paper scrim at 55% and a card 20 at the sides, 18 above and below. |

## Defaults

Every app starts with Illustration, a colour per part, ghosts as shells, a shadow, grain and no
wobble: `geo_default_look()`, which the Mac reads, the page asks the engine for, and the tools use.
