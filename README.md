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
| 1, 2, 3, or Face, Body, Part in the toolbar | What a click selects: one face, a whole body or a whole part |
| Hold H and click | Hide what you click. Repeated clicks peel away layers |
| Hold G and click | Ghost what you click. Clicking a ghost with G held brings it back |
| ⇧H, ⇧G | Show everything hidden, unghost everything |
| I | Isolate the selection: everything else hides and the view frames it, as if it alone were open. Rotation turns about it. When everything is shown again, the view returns to where it was |
| ⌘Z, ⇧⌘Z | Undo and redo hiding and ghosting |
| Left drag | Rotate |
| Scroll, pinch | Zoom toward the pointer |
| Middle drag, or Shift or Space with left drag | Pan |
| Right drag | Dolly |
| F | Fit to view |
| P | Switch between orthographic and perspective |
| Esc | Clear the selection |
| T | See through the housing to the mechanism inside |
| S | Cut a section across X; the scissors button opens all three axes |
| Click the view cube | Turn to that face's view. Standard views are also in the View menu |
| ? | Show every shortcut |
| Drag the top edge or the file's name | Move the window. Double-click to zoom it, as with a title bar |

The palette button in the toolbar, or ⇧⌘L, opens the look: five drawing styles (Illustration,
Metal, Clay, Blueprint and Technical), what decides each part's colour (its material, read from
its name, one colour per part, one throughout, or the file's colours), how ghosts look (a faint
shell, dashed lines or both), the cast shadow, paper grain and a hand-drawn wobble. The same
choices are in the View menu, and the look is remembered.

The toolbar at the bottom switches the selection mode and holds the hide and ghost tools, and the
card on the right shows what is selected, with buttons to hide, ghost, isolate or fit to it.

The left card switches between Library and Structure. Connect Folder (⌥⌘O) adds folders to the
library, which lists their STEP and STL files and remembers them across launches. Open files that
change on disk reload. Settings (⌘,) rebinds every key and what each mouse button drags.

The Structure card lists each open file's assembly tree. Each part shows a swatch of the colour it
is drawn in, each assembly how many parts it holds, and a part of several bodies how many it has.
Click a row to select it, hover a row to highlight it, and type in the search field to find parts
by name. A row under the pointer has buttons to ghost, hide and isolate it, and hidden or ghosted
rows say so. While anything is hidden or ghosted, the foot of the card counts it and offers Show all
and Unghost. Fit and rotation always follow what is shown. ⇧⌘O adds a file to what
is shown, and ⌘-dropping files does the same.

## The universal viewer, and AI apps

`Launcher/` runs the viewer on any computer with Node 20 or later: it serves the page, its
WebAssembly engine and its importers on 127.0.0.1 only, and opens it in the browser. Nothing leaves
the machine; there is no account or hosted site.

```sh
cmake --build --preset wasm-webgpu && cmake --build --preset wasm-release && \
    cmake --build --preset wasm-threads
node Launcher/bin/geometry-viewer.mjs          # serves the viewer and opens the browser
```

The same launcher is a tool server for AI apps (MCP). It lets Claude, Codex or ChatGPT open a
STEP or STL file on this computer, list its assemblies and parts, render pictures of it (any view
or style, parts alone, sections), and show it to the person. On a Mac with the release preset
built, it uses the native engine (`gcli`) and the Geometry app; anywhere else it opens the viewer
in the browser and works through it, so the person sees what the AI looks at.

- **Claude Desktop:** `node Launcher/scripts/pack.mjs`, then double-click
  `Launcher/dist/geometry-viewer.mcpb`. It carries the viewer, and on a Mac the native engine.
- **Codex or ChatGPT desktop:** add to `~/.codex/config.toml`:

  ```toml
  [mcp_servers.geometry]
  command = "node"
  args = ["<repo>/Launcher/bin/geometry-viewer.mjs", "--mcp"]
  ```

  Or, from a packed copy (`cd Launcher && npm pack`), `command = "npx"` with
  `args = ["-y", "--package=<path>/geometry-viewer-0.1.0.tgz", "geometry-viewer", "--mcp"]`.
- **Claude Code:** `claude mcp add geometry -- node <repo>/Launcher/bin/geometry-viewer.mjs --mcp`.

`GEOMETRY_ENGINE=page` makes a Mac use the browser's viewer too, and `--port N` fixes the port.
`cd Launcher && npm test` checks the server and drives the tools as an AI app does.

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
    1600 1000 35 25 ortho 1 0 x0.5 plain   # size, view, projection, style, colour, section
```

A section can be added as `x0.5` or `x0.6,y-0.4` (a minus removes the low side), and `plain`
leaves out the view cube and scale bar.

`Tests/golden` holds an image of every style and colour mode. After a deliberate change to how
things look, `GEOMETRY_UPDATE_GOLDENS=1 build/debug/Tests/geo_tests -tc="*golden*"` rewrites them,
to be checked by eye before committing.

`Tools/step_equivalence.py` compares STEP imports with OCCT reading each file whole, on face count,
area, volume and bounds:

```sh
Tools/step_equivalence.py build/release files.txt --jobs 6
```

The `asan` and `tsan` presets run the same tests under sanitizers.

`Tools/perf_gate.py build/release` imports the fixtures and fails if any got more than 5% slower
than `Tools/perf_baseline.json`; `--update` records a new baseline. `Tools/fuzz.sh 600` fuzzes the
STEP tokenizer, the STL reader and the cache reader for ten minutes each.

Setting `GEOMETRY_TRACE=1` prints stage timings, memory use and GPU time per frame, `GEOMETRY_LATENCY=1` measures
input-to-present latency, and `GEOMETRY_ALLOCS=1` counts per-frame heap allocations. Launching with
`--exercise` runs a scripted drag, hover and click on the opened file, then checks that changing only
the interface draws no 3D frame. `--chrome-check` clicks every segment and card label with real
events and checks that none of them reaches the 3D view. `--snapshot` writes the window, engine output and interface
together, to `chrome.png` in the app's temporary directory.

## Project goals, in order

1. Speed
2. Memory
3. Leanness

Where speed and memory conflict, speed wins.
