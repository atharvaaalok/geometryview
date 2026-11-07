# ADR 0004: Per-definition position grid with optional high bits

Status: accepted

## Context

Spike 1 stored positions as 16-bit integers on one grid per definition. That gives extent/65535
precision, about 61 µm on a 4 m part, which is too coarse for dense scans and for large single-body
files that are viewed close up.

The plan proposed a per-meshlet origin plus a 16-bit delta on a 24-bit grid. That does not work for
CAD: a delta spans 1/256 of the definition, and large planar faces and long cylinders routinely span
the whole part, so most CAD meshlets would overflow it. A per-meshlet shift would fit, but vertices on
a border between meshlets with different shifts would land on different grid points, leaving cracks.

## Decision

Every vertex in a definition sits on one integer grid, in one of two precisions:

- **Compact:** a 16-bit grid. Vertices are 12 bytes.
- **Wide:** a 24-bit grid. Vertices are the same 12-byte record plus a parallel 4-byte stream holding
  the high 8 bits of each coordinate, so 16 bytes in total.

A definition is wide when its median sampled edge is under 8 steps of the 16-bit grid. Compact is
fine for default CAD tessellation, because `MinSize` is about 98 steps of that grid.

Wide stops at 24 bits because a float's mantissa represents those integers exactly. Two vertices at
the same grid point therefore decode to identical floats in any meshlet, so shared edges stay
watertight. Decoding reads one extra word per vertex only for wide definitions.

## Consequences

- A definition's precision is a single flag, so the common case costs nothing.
- Meshlet records hold no origin and stay at 32 bytes. Their bounds use a 16-bit grid, which is rounded
  up to stay conservative.
- `localFace` is 16 bits relative to a per-meshlet `faceBase`. Packing renumbers faces in spatial order
  and limits each work unit to 2^16 faces, so a definition can hold any number of faces. The file keeps
  a table back to the original face ids.
