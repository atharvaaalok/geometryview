# Geometry

A native macOS viewer for STEP, STL and other geometry files, built for extreme speed and low
memory use. Open your own files or connect a whole directory, then browse, inspect, section and
see through them.

Status: early development. See [Docs/PLAN.md](Docs/PLAN.md) for the design and milestones, and
[Docs/adr](Docs/adr) for recorded decisions.

## Requirements

- macOS 26 or later on Apple silicon
- Xcode 27 with the Metal Toolchain component
- CMake, Ninja and LLVM (`brew install cmake ninja llvm`)
- OCCT, for STEP files: run `ThirdParty/build_occt.sh` once, which builds the pinned version in
  about five minutes. Without it everything else still builds, and STEP files will not open.

## Build and run

```sh
cmake --preset release
cmake --build --preset release
open build/release/App/Geometry.app
```

Drop a STEP or STL file on the window, or press ⌘O. Large STEP files appear while they import, and
parts you zoom into are retessellated more finely once the view settles.

| Input | Action |
|---|---|
| Hover | Highlight the face, body or part under the pointer |
| Left click | Select it. Click empty space to clear. Shift or ⌘ adds to the selection |
| 1, 2, 3 | Select faces, bodies or parts |
| Hold H and click | Hide what you click. Repeated clicks peel away layers |
| Hold G and click | Ghost what you click. Clicking a ghost with G held brings it back |
| ⇧H, ⇧G | Show everything hidden, unghost everything |
| I | Isolate the selection |
| ⌘Z, ⇧⌘Z | Undo and redo hiding and ghosting |
| Left drag | Rotate |
| Scroll, pinch | Zoom toward the pointer |
| Middle drag, or Shift or Space with left drag | Pan |
| Right drag | Dolly |
| F | Fit to view |
| P | Switch between orthographic and perspective |
| Esc | Clear the selection |
| Click the view cube | Turn to that face's view. Standard views are also in the View menu |
| ? | Show every shortcut |

The palette button in the toolbar, or ⇧⌘L, opens the look: five drawing styles (Illustration,
Metal, Clay, Blueprint and Technical), what decides each part's colour (its material, read from
its name, one colour per part, one throughout, or the file's colours), how ghosts look (a faint
shell, dashed lines or both), the cast shadow, paper grain and a hand-drawn wobble. The same
choices are in the View menu, and the look is remembered.

The toolbar at the bottom switches the selection mode and holds the hide and ghost tools, and the
card on the right shows what is selected, with buttons to hide, ghost, isolate or fit to it.

The Structure card on the left lists each open file's assembly tree. Click a row to select it, hover
a row to highlight it, and type in the search field to find parts by name. ⇧⌘O adds a file to what
is shown, and ⌘-dropping files does the same.

## Development

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # unit, GPU and layering tests
```

`gcli` imports, checks and renders files without the app:

```sh
build/release/Tools/gcli import part.step part.geoblob build/release/Import/geometry-occt-worker
build/release/Tools/gcli stats part.geoblob
build/release/Tools/gcli render build/release/shaders/default.metallib part.geoblob part.png \
    1600 1000 35 25 ortho 1 0   # size, azimuth, elevation, projection, style, colour mode
```

`Tests/golden` holds an image of every style and colour mode. After a deliberate change to how
things look, `GEOMETRY_UPDATE_GOLDENS=1 build/debug/Tests/geo_tests -tc="*golden*"` rewrites them,
to be checked by eye before committing.

`Tools/step_equivalence.py` compares STEP imports with OCCT reading each file whole, on face count,
area, volume and bounds:

```sh
Tools/step_equivalence.py build/release files.txt --jobs 6
```

The `asan` and `tsan` presets run the same tests under sanitizers.

Setting `GEOMETRY_TRACE=1` prints stage timings, memory use and GPU time per frame, `GEOMETRY_LATENCY=1` measures
input-to-present latency, and `GEOMETRY_ALLOCS=1` counts per-frame heap allocations. Launching with
`--exercise` runs a scripted drag, hover and click on the opened file, then checks that changing only
the interface draws no 3D frame. `--snapshot` writes the window, engine output and interface
together, to `chrome.png` in the app's temporary directory.

## Project goals, in order

1. Speed
2. Memory
3. Leanness

Where speed and memory conflict, speed wins.
