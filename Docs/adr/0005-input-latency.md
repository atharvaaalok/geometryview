# ADR 0005: Start frames just before the commit deadline

Status: accepted. Spike 9 **fails its target at 60 Hz and at 120 Hz**.

## Context

Spike 9 set a target of p95 ≤25 ms from input to photon. There is no camera on the display here, so
this measures from the input event's timestamp to the time Metal reports for the present. That leaves
out the mouse's own latency and the panel's response, so real input-to-photon is higher.

The measurement uses the oldest input waiting for each frame, which is the worst case in that frame.
Input arrives from a scripted 240 Hz drag posted at the engine's public interface, so AppKit's own
event delivery is also excluded. Results come from `--exercise` on a release build, with the 1M-triangle
car, 6 s of dragging, and `GEOMETRYVIEW_LATENCY=1`.

This Mac's displays both report a 60 Hz maximum, including the built-in ProMotion panel.

## Results

| Present path | Median | p95 |
|---|---|---|
| `CAMetalDisplayLink`, render at once | 63.5 ms | 64.1 ms |
| **`CAMetalDisplayLink`, start 4 ms before the commit deadline** | **53.2 ms** | **53.8 ms** |
| `CADisplayLink`, then `nextDrawable` | 64.8 ms | 65.1 ms |

Late start, on other displays:

| Display | Median | p95 |
|---|---|---|
| Built-in, fullscreen | 51.3 ms | 52.0 ms |
| External 3440×1440 at 1× | 50.9 ms | 53.8 ms |

Where the time goes at 60 Hz (late start):

| Stage | Time |
|---|---|
| Oldest input to frame start | about 15–17 ms, one frame interval by construction |
| Frame start to commit deadline | about 3 ms, the start budget |
| Commit deadline to intended present | 33.3 ms, on every path and display |
| Intended to actual present | 0.0 ms |

## Decision

Use `CAMetalDisplayLink` and start each frame 4 ms before its commit deadline. Encoding took 0.15–0.32 ms
on these scenes, so rendering at once left a finished frame waiting a whole interval while newer input
arrived. A re-armed timer rather than a sleep lets the run loop keep draining input, and pacing allocates
nothing. Starting late did not drop frames. The two losing paths were removed, and they remain in git
history.

## Why the target is missed

The 33.3 ms from commit to present is exactly two frames, and it did not change on any present path, in
fullscreen or on the external display. It appears to be a fixed floor in the system's compositor, though
that cause is inferred. No setting tried here reduced it. The floor alone exceeds the 25 ms target at
60 Hz, so the target cannot be met there.

The floor is measured in frames, so 120 Hz should halve it, to perhaps 25–29 ms by the same measure. That
is an extrapolation, not a measurement.

The planned third path, presenting an IOSurface through a `CATransaction`, was not tried. Its evidence
was one project's unverified numbers, and it would pass through the same compositor.

## Update: 120 Hz, measured 2026-11-07

The built-in display later reported 120 Hz, and the pack fixture ran at 119.3–119.8 fps.

| Setting | Median | p95 |
|---|---|---|
| Windowed, `preferredFrameLatency` 1 | 43.9 ms | 44.0 ms |
| **Fullscreen, `preferredFrameLatency` 1** | **41.6 ms** | **41.6 ms** |
| Windowed, `preferredFrameLatency` 2 | 45.1 ms | 45.2 ms |
| Windowed, `preferredFrameLatency` 0.5 | 42.4 ms | 42.8 ms |

Commit deadline to intended present stayed at **33.3 ms** in every case. At 120 Hz that is four
frames, not two, so the floor is a fixed time rather than a number of frames, and the extrapolation
above was wrong. Input to frame start fell to 5–8 ms, half a 120 Hz interval, which is the only part
the refresh rate shortened. Within the frame link's settings the floor did not move. The target stays
out of reach on this path. Finding what sets the 33.3 ms, such as a compositor queue, needs a
different present path or instrumentation, and is left open.

## Consequences

- The 120 Hz numbers above replace the extrapolation.
- Hover uses the same path, so it should behave the same. It was not measured separately. Its state
  updates when the event is processed, it draws on the next frame, and it reaches the display about two
  frames later.
