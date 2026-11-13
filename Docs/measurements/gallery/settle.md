# How a folder settles

What a viewer sees while a folder's gallery fills in: how often parts already drawn move, go
(a cover swaps one part for another) or have their scale bar change, and when the last change
comes. Measured by the gallery's settle checks, which sample where each part is drawn every 50 ms
(its own box, from its picture's note, or its measured box while drawn in construction lines)
until nothing in view waits and nothing has changed for 3 s, at most 90 s:

```sh
# Mac: warm, then cold (an empty cache in the app's container)
open -g -n -W --stdout out.txt -a build/release/App/GeometryView.app --args \
  --gallery "$PWD/test-models" --gallery-settle warm
open -g -n -W --env GEOMETRYVIEW_GALLERY_CACHES=~/Library/Containers/com.aalok.geometryview/Data/tmp/settle-cold-cache \
  --stdout out.txt -a build/release/App/GeometryView.app --args --gallery "$PWD/test-models" --gallery-settle cold
# gallery-settle.json in the app's temporary folder

# Web: always cold, as each run's browser profile starts empty
GEOMETRYVIEW_GALLERY_FOLDERS="test-models=$PWD/test-models" node Tools/web_report.mjs \
  "/viewer/?test=gallery&backend=visit&folder=test-models&settle=cold&label=local-settle-cold" 200
```

The folder is test-models: 1,862 STEP and STL files in four folders of folders, shown in Plates
at its root, as the recording that prompted this showed it.

## Baseline (2026-11-13, 0.5.0 plus the ink fixes)

| | first part | changes | moved | went | scale bars | last change |
|---|---|---|---|---|---|---|
| Mac, warm (facts and pictures kept) | 0.05 s | 46 | 108 | 62 | 0 | 3.2 s |
| Mac, cold (nothing kept) | 0.27 s | 286 | 1,296 | 642 | 13 | 34.6 s |
| Web, cold | 0.25 s | 40 | 146 | 15 | 4 | still at 90 s |

- Mac, cold: 1,846 structure passes, 1,123 imports and 1,128 pictures in 37 s; 240 imports were
  cancelled when the layout changed which files were in view.
- Mac, warm: 1,862 look-ups and no picture drawn; all facts and pictures arrive within 0.15 s,
  but each picture was laid out only once decoded, swapping its part's box for its outline, so
  covers re-picked and rescaled for 3.2 s (63 layouts). A recording made just after 0.5.0 was
  installed, when every picture was drawn again (pictures were keyed to the app's version),
  changed for 33 s.
- The first version of the check polled every 50 ms and starved while the gallery laid out
  again and again: it saw the warm open change once, at 2.4 s. It now records every layout.

## After each fix

| | layouts | changes | moved | went | scale bars | last change |
|---|---|---|---|---|---|---|
| Mac, warm: a first picture laid out from its note at once | 5 | 3 | 17 | 100 | 0 | 0.16 s |
| Mac, warm: covers from fixed candidates, kept; strips in fixed slots | 4 | 0 | 0 | 0 | 0 | none |
| Mac, cold: the same | 360 | 67 | 72 | 2 | 3 | 17.3 s |
| Mac, warm: arrivals settle in steps (0.1 s after a burst begins, then every 0.75 s at most) | 4 | 1 | 7 | 1 | 0 | 0.11 s |
| Mac, cold: the same | 281 | 11 | 30 | 1 | 1 | 13.9 s |
| Web, cold: candidates smallest first, then settling in steps | – | 3 | 35 | 1 | 0 | 51.8 s |

What a warm open still did then was all in its first 0.16 s, as facts arrive 128 look-ups at a
time: covers re-picked from the parts measured so far, and strips stepped through them. With each
cover drawn from twelve candidates asked for first, keeping the parts it shows and a scale that
only coarsens until a natural moment, and each strip slot keeping its file, a warm open lays out
four times and changes nothing; a cold one no longer swaps parts out (2 went, from 642), and with
nothing it shows reshuffled no import is cancelled (from 240). What a cold open still moves is a
folder's own shelf taking in each newly measured part in size order, and a part's box giving way
to its outline when its picture comes.
- Web, cold: one import at a time, so the first change comes at 23.7 s and changes go on past
  90 s.

Settling in steps: facts, failures and a file's first picture wait in the core until the front
end settles, 0.1 s after the first arrival of a burst, then at most every 0.75 s while it goes
on (a picture replacing one shown, and what a rescan re-applies, apply at once). A cold open of
test-models now changes 11 times instead of 67, and a warm one once, in its first 0.11 s, as the
last look-ups land. On the web, which reads one file at a time, a cold open changes 3 times in
90 s; it first did 40, still changing at 90 s.
