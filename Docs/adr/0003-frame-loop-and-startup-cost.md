# ADR 0003: On-demand frame loop, and what startup really costs

Status: accepted, with spike 9 and the present-path comparison carried into M1

## Decision

A dedicated engine thread is the single writer of view state and renders from a `CAMetalDisplayLink`
with `maximumDrawableCount = 2` and `preferredFrameLatency = 1`. The link pauses 250 ms after the last
activity, because ProMotion lowers its refresh rate when presents stop, and an idle window then costs
nothing. Commands reach the engine through a lock-free ring, so the main thread never blocks.

## M0 results

Measured on a release build, on this M4 Max, on the internal display at 1440×900 points.

| Measure | Target | Result |
|---|---|---|
| Process start to first frame presented, warm | ≤200 ms | 125–146 ms, 5 runs |
| Same, first launch after a rebuild | n/a | 532 ms, 1 run |
| Idle CPU | 0 | 0.0% |
| Idle wakeups | 0 | 0 in each `top` sample |
| Settled footprint | n/a | 67–72 MB, of which 40 MB is two drawables |
| Executable | n/a | 0.1 MB |

The 532 ms run is one sample, probably first-launch signature and dyld work on a new binary. The cause
was not investigated.

## The render-pass footprint spike

Peak footprint was 296 MB while the settled value was 67 MB. Startup was traced to find out why, since
memory is a top priority.

- A bare AppKit window peaks at 18 MB, so neither macOS windows nor the app's chrome cause it.
- Metal device, queue, allocator and layer setup cost under 6 MB. Encoding the first frame adds 1.3 MB.
- `vmmap` regions sum to about 90 MB both during and after the spike, so the memory is not in any
  region mapped into the process.
- A single offscreen render pass with no window reproduces it. A blit does not:

| Render target | Before | After one render pass | 4 s later |
|---|---|---|---|
| 64×64, blit only | 4.6 MB | 5.0 MB | 5.0 MB |
| 64×64 | 4.4 MB | 247.8 MB | 10.6 MB |
| 1440×900 | 4.4 MB | 254.1 MB | 16.9 MB |
| 2880×1800 | 4.4 MB | 274.8 MB | 37.6 MB |
| 3456×2234 | 4.4 MB | 288.8 MB | 51.6 MB |

So the first render pass adds about 240 MB regardless of resolution, and it is released within about
4 s of idle. This is consistent with driver-owned working memory for tile-based rendering, but that
explanation is inferred, not confirmed. No API was found to change it.

## Consequence for the plan

The plan's viewer memory target omitted this. While actively rendering, expect about **240 MB of
driver memory** on top of render targets and other anonymous memory. It is not caused by this code and
falls away shortly after rendering stops, so on-demand rendering recovers it automatically. The
target should be read as:

- Rendering: about 240 MB driver memory + 134 MB render targets + under 150 MB other, at 3456×2234.
- Idle a few seconds later: render targets + under 150 MB other, with geometry in file-backed pages.

## Carried into M1

The plan's M0 exit criterion asked for present paths to be measured on both displays, and spike 9 asks
for input-to-photon. Neither is meaningful until input moves a real camera, so both move to M1. Without
a camera on the display, latency can only be measured from the input event to the reported present
time, which excludes the display itself. This limit will be stated with the result.

`screencapture` needs Screen Recording permission, so visual checks use `--snapshot` for chrome and
`gcli render` for 3D.
