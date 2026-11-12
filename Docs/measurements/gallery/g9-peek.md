# G9: the turnable Peek

Measured on 2026-11-11 (afternoon, Pacific Time) on the M4 Max, on designlib's output, in G3's
checks: Peek opened on the first file of the first subfolder in Sheet (`all-sizes`, an assembly of
108 parts, 1162 × 84.7 × 72 mm), then turned a quarter with →.

| Front end | Frames | Ready after Peek opened | Peek aligned, turnable | After six → |
|---|---|---|---|---|
| Mac | 24, 1024 px square | 0.95 s | 41 groups, 0 over 0.5 pt, 0 pt off | frame 6, same file |
| Chrome | 24, 768 px square | within 1.5 s | 35 groups, 0 over 0.5 pt, 0.023 pt off | frame 6, same file |

- **One scale for every turn.** Each frame holds the scale at which the part's box diagonal fits
  any way it faces, so the part does not grow and shrink as it turns. Each front end fits every
  frame alike into Peek's picture area by the box holding all their ink, as the board does.
- **Turning.** A drag across the part turns it a frame every 24 pt, under a left-right pointer; ←
  and → turn it a frame, while ↑ and ↓ still go file to file. Until the frames have come, Peek
  shows its still picture and the arrows move as before.
- **Kept.** The Mac keeps frames beside the pictures: a turntable asked for again is read back,
  not drawn (a unit test checks the count). The browser draws them again each time.
- **Its cache file.** A file whose picture came from the gallery's cache may have no cache file
  left: the turn imports it first, at the open band.
- **Found on the way.** Arrows moving Peek set its file directly, past what happens when Peek's
  file changes; the next file would have taken the last one's turntable. They now go through it.
- In the browser, the frames' box was first grown as if its corners were a size, which drew the
  part as a speck; the shot showed it, and it was fixed before the run above.
