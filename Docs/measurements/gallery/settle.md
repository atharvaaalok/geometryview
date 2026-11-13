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

What a warm open still does is all in its first 0.16 s, as facts arrive 128 look-ups at a time:
covers re-pick from the parts measured so far, and strips step through them.
- Web, cold: one import at a time, so the first change comes at 23.7 s and changes go on past
  90 s.
