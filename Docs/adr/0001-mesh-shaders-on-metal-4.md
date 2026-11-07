# ADR 0001: Render with wide mesh shaders through the Metal 4 API

Status: accepted (spikes 1 and 3)

## Context

The plan bet on object and mesh shaders, but planning data came from uniform synthetic spheres and
predicted that mesh shading would lose below about 60% meshlet fill. It also assumed a 64-vertex,
124-triangle meshlet. Spike 1 had to prove this on real geometry, and spike 3 had to show that
Metal 4 encoders stay within 5% of Metal 3.

## Method

`Tools/spikes/geometry_path.mm`, release build, offscreen at 3456×2234 with memoryless 4× MSAA on an
M4 Max 40-core GPU, on AC power. Each row is the median GPU time of 24 frames after 6 warm-up frames.
Every image is diffed against the classic pipeline.

Real data: the 13S4P battery pack as tessellated by step_viewer (852,428 triangles, per-face split
vertices, repeated 14 times to reach 11.9 Mtri) and a 10.0 Mtri welded binary STL.

## Results

Time relative to the classic indexed pipeline in the same run, at a 124-vertex limit:

| Data | Zoom | Classic | Mesh, no culling | Mesh, culling |
|---|---|---|---|---|
| Pack ×14 | 1× | 2.01 ms | 0.88× | 0.82× |
| Pack ×14 | 2.5× | 2.32 ms | 0.90× | 0.84× |
| Pack ×14 | 5× | 2.23 ms | 0.91× | 0.64× |
| 10M STL | 1× | 1.86 ms | 0.86× | 0.74× |
| 10M STL | 2.5× | 2.02 ms | 0.84× | 0.72× |
| 10M STL | 5× | 1.43 ms | 0.77× | 0.47× |

Meshlet fill by vertex limit:

| Data | Limit | Triangle fill | Vertex fill | Meshlets | Vertex duplication |
|---|---|---|---|---|---|
| Pack | 64 | 49.1% | 99.9% | 13,987 | 1.04× |
| Pack | 96 | 74.6% | 99.9% | 9,213 | 1.03× |
| Pack | 124 | 96.3% | 99.2% | 7,141 | 1.03× |
| 10M STL | 64 | 78.6% | 100.0% | 102,620 | 1.31× |
| 10M STL | 124 | 100.0% | 64.3% | 80,646 | 1.29× |

Metal 4 against Metal 3 for the same mesh pipeline, once clocks had settled: 1.77 vs 1.78 ms,
1.59 vs 1.60 ms, 2.02 vs 2.02 ms, 1.44 vs 1.43 ms. No measurable difference.

Image correctness: the STL matches classic exactly (0 differing pixels). The pack differs in 1 or 2
of 2–4 million covered pixels, by at most 23/255. That size fits depth ties between coincident faces
drawn in a different order, though the cause was not confirmed separately.

## Decision

- Use object and mesh shaders with one thread per primitive as the only 3D path.
- Use a meshlet limit of **124 vertices and 124 triangles**, not 64 and 124. CAD meshes split vertices
  per face, so the vertex count is about equal to the triangle count and 64 vertices strands half of
  the triangle capacity.
- Use the Metal 4 API. Spike 3 passes on parity, not on speed. The reasons to choose it are argument
  tables, explicit residency sets that fit chunked file mappings, and flexible pipeline states.

## Consequences and limits

- The classic pipeline is not needed as a fallback on this GPU.
- Timing noise is large: the same classic run measured 1.99 to 2.84 ms depending on when it ran in
  the process, because GPU clocks ramp. Only compare results taken after clocks settle. An early run
  of this spike appeared to show Metal 4 about 10% faster, which was this artefact.
- Meshlet generation took 3.9 s for 10M triangles on one thread, which alone exceeds the M1 budget of
  1.5 s for the 500 MB STL, so it must be parallelised.
- Burst timings only. Sustained thermals are spike 5, and Apple7/8 GPUs are spike 2.
