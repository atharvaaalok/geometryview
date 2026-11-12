# Fixes from the first look at the gallery

Found by the user trying the debug app (2026-11-11, afternoon, Pacific Time), fixed on branch
`gallery-fixes`, and measured on designlib's output in 1440 × 900 (Mac) and 1440 × 757 or 2560 ×
1440 (Chrome, headless) with all five views.

| What | Mac | Chrome |
|---|---|---|
| Esc with the Look card open | card closed, gallery still up | card closed, gallery still up |
| × in the gallery | gallery left, its view hidden | gallery left, its view hidden |
| Another connected folder, then back | all 166 files' facts back in 0.6 s, 0 waiting | (the browser reset already) |
| Detail pictures | Catalogue's figure: 1 wanted, 1 shown in 0.2 s | 1 wanted, 1 shown (2560 × 1440) |
| Alignment, every view and style | 0 groups over 0.5 pt | 0 groups over 0.5 pt |

- **Vanishing parts** came from showing another connected folder and coming back: the gallery
  dropped what it knew, and the library service never told a file twice. `geo_library_reset` now
  makes it tell again, from its caches; a unit test shows nothing is told without it.
- **Shadows over the rule:** Plates' covers fit each row with its deepest shadow and stand it
  that much higher; Ladders' parts stand 1.75 pt above the ruler's line. A unit test checks every
  picture's ink against the rule or ruler under it (22 crossed before the fix).
- **Sharpness:** supersampled pictures are less than half as far from a picture drawn at four
  times (0.55 against 1.19 mean per channel). A 512-px picture takes 39 ms supersampled against
  8.6 ms plainly in a debug build. Every picture is drawn once more, under its new key.
- **The browser's check** sent its Esc to the window itself, where Chrome runs the window's
  listeners in the order they were added; a key pressed by a person is aimed at the page's focus,
  where the gallery's capturing listener comes first. The check now presses it there.
