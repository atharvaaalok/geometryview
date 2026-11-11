# G9: posters for every view, and PDF

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max:
- the Mac app's debug build, 1440 × 900, with `--gallery-check --gallery-poster --gallery-views all`;
- Chrome 157 headless, WebGPU on Metal, at 1440 × 900, with `?poster=1&views=all`.

Each check shows the folder as a poster in all five views, measures every alignment group as
painted, draws it as a PNG at 2 pixels a point, and makes its PDF: one page, or in Catalogue the
whole book, a spread a page.

## Alignment, as painted

| Folder | Front end | Groups over 0.5 pt, all five posters | Furthest from the layout |
|---|---|---|---|
| designlib's output | Mac | 0 | 0 pt |
| designlib's output | Chrome | 0 | 0.134 pt (Atlas), else ≤ 0.026 pt |
| test-models | Mac | 0 | 0 pt |
| test-models | Chrome | 0 | 0.134 pt (Atlas), else ≤ 0.028 pt |

## The posters

| Folder | View | Mac: height, PNG | Mac PDF | Chrome PDF |
|---|---|---|---|---|
| designlib's output | Plates | 862 pt, 2880 × 1843 px | 5.2 MB, 0.3 s | 0.4 MB, 0.05 s |
| | Sheet | 3,031 pt, 2880 × 6182 px | 9.4 MB, 0.6 s | 1.6 MB, 0.13 s |
| | Ladders | 4,158 pt, 2880 × 8436 px | 9.5 MB, 0.7 s | 1.9 MB, 0.13 s |
| | Atlas | 840 pt, 2880 × 1800 px | 4.0 MB, 0.2 s | 0.6 MB, 0.04 s |
| | Catalogue | 18 spreads | 12 MB, 1.2 s | 5.4 MB, 0.6 s |
| test-models | Plates | 862 pt | 11 MB, 0.5 s | 0.4 MB, 0.07 s |
| | Sheet | 8,138 pt, 2880 × 16395 px | 121 MB, 5.6 s | 6.6 MB, 0.6 s |
| | Ladders | 27,298 pt, 1777 × 33762 px | 141 MB, 6.4 s | 5.1 MB, 0.7 s |
| | Atlas | 840 pt | 4.9 MB, 1.0 s | 0.4 MB, 0.04 s |
| | Catalogue | 57 spreads | 154 MB, 36 s | 24 MB, 184 s |

- **Ladders** prints every family on one plate: each strip wraps, at one scale with the typical part
  about 34 pt tall, unless the families differ more than 20 times in size (both folders here):
  then each keeps its own scale, stamped, and the title block says "as stamped". A frame of two
  rules and a drafting title block close it.
- **Atlas** prints the whole map in the same frame beside a cartouche: compass, title, counts, a
  legend of its regions, and how its areas are drawn. The first shots had district names drawn over
  by the boxes inside them: at the fitted zoom their heads, in map units, are too small for names
  kept at their size on screen. The poster's map is now laid out narrower in map units, so it is
  drawn at 0.75 pt a unit, past where districts show live.
- **Catalogue**'s poster is its open spread; its PDF is the whole book.
- **Very large posters** are drawn smaller rather than failing: test-models' Ladders poster, 27,298
  pt tall, became a 1777 × 33762 px PNG (60 million pixels at most; a browser's canvas holds 32,767
  a side). A PDF page over 14,400 pt, the most a PDF reader takes, is scaled down whole.
- The Mac's Ladders poster of test-models is twice as tall as Chrome's: a strip's slot is as wide
  as its part's name, and the Mac reads more product names inside the files (G8's open item).

## The PDFs

- **Mac:** drawn from the gallery's own layers into a PDF context: text stays text in IBM Plex
  Mono, lines and boxes stay vector, pictures stay pictures. Core Graphics keeps every picture
  losslessly, so a PDF of many parts is large: 154 MB for test-models' book.
- **Chrome:** pages of pictures, each the poster drawn on a canvas as Save PNG draws it and kept as
  a JPEG (`Web/viewer/pdf.js`, no dependency). Smaller, but the text is not text.
- **Time:** a book waits for each spread's pictures, 20 s at most a spread. Chrome took 184 s for
  test-models' 57 spreads, most of it waiting for pictures not yet drawn; the Mac, whose were drawn,
  36 s. While it works, the masthead (Mac) or the Save PDF button (Chrome) counts the pages, and a
  second export waits.
- Both open in Preview and Quick Look at their own page sizes. A node test reads the browser's
  cross-reference table back to every object.

## Tests

- `ctest --preset debug`: Ladders' and Atlas's posters, aligned in two styles, with nothing outside
  their frames, no lanes on paper and every file once.
- `npm test` in Launcher: the browser's PDF writer.
- Reports: `web/mac-chrome-g9-gallery-{designlib,test-models}.json`.
