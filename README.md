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

## Build and run

```sh
cmake --preset release
cmake --build --preset release
open build/release/App/Geometry.app
```

Drop an STL file on the window, or press ⌘O. STEP support is in progress.

| Input | Action |
|---|---|
| Hover | Highlight the surface under the pointer |
| Left click | Select a surface. Click empty space to clear |
| Left drag | Rotate |
| Scroll, pinch | Zoom toward the pointer |
| Middle drag, or Shift or Space with left drag | Pan |
| Right drag | Dolly |
| F | Fit to view |
| P | Switch between orthographic and perspective |
| Esc | Clear the selection |

## Development

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # unit, GPU and layering tests
```

The `asan` and `tsan` presets run the same tests under sanitizers.

`gcli` works without the app:

```sh
build/release/Tools/gcli import model.stl model.geoblob
build/release/Tools/gcli render build/release/shaders/default.metallib model.geoblob out.png
```

Setting `GEOMETRY_TRACE=1` prints stage timings and memory use, `GEOMETRY_LATENCY=1` measures
input-to-present latency, and `GEOMETRY_ALLOCS=1` counts per-frame heap allocations. Launching with
`--exercise` runs a scripted drag, hover and click on the opened file, and `--snapshot` writes the
interface to a PNG.

## Project goals, in order

1. Speed
2. Memory
3. Leanness

Where speed and memory conflict, speed wins.
