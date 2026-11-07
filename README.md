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

## Project goals, in order

1. Speed
2. Memory
3. Leanness

Where speed and memory conflict, speed wins.
