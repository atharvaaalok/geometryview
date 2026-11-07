# ADR 0008: Chrome in Core Animation layers, view aids in Metal

Status: accepted. M4's exit checks pass.

## Decision

The interface is split by what it must stay in step with.

- **The cards, masthead, toolbar, hint, drop frame and shortcut sheet** are custom AppKit views drawn
  with `CALayer`s, above the `CAMetalLayer` and never inside it. Changing them is Core Animation work
  only, so it costs no 3D frame. Hard shadows are a second rounded rectangle offset by 3 points, not a
  blurred shadow, so no offscreen pass is needed.
- **The view cube and scale bar** are drawn by the engine at the end of the main pass, so they move in
  lockstep with the camera. The cube writes ids above `0xFFFFFF00`, and picking it uses the same ID
  buffer read as the model. Labels come from a Core Text atlas in IBM Plex Mono.
- **Framing respects the chrome.** The window reports, in points, the edges its cards cover. Fit, fit
  to selection and the standard views size the model to the rectangle left clear, at least a third of
  the viewport each way, and centre it there.
- **The engine tells the app only what changed.** It emits a hover event when the part under the
  pointer changes, not per frame. It publishes a selection summary (bodies, faces and the tree row)
  whenever the state is published.

The controls are a finite set built once: pill button, segmented pill, chip and key cap. The motion
tokens are the `cubic-bezier(.3,.7,.2,1)` ease at 160 ms and 220 ms, and every transition is instant
under Reduce Motion. The colours carry the meanings from the plan: cobalt for selection, cream for
hover, tomato for about to hide, and mustard for progress.

## Results

Measured on a release build on this M4 Max, on the built-in display at 120 Hz.

| Check | Target | Result |
|---|---|---|
| 3D frames drawn by 12 chrome-only changes | 0 | 0 |
| Idle CPU, pack open, all chrome showing | 0 | 0.0% in each of 4 `top` samples |
| Idle wakeups, same | 0 | 0 in each sample |
| Idle footprint, same | n/a | 148 MB |
| Frame rate while dragging PLE120 with chrome showing | display rate | 119.5 fps |
| Input to present while dragging | no regression from ADR 0005 | median 42.9 ms, p95 43.6 ms |

The chrome-only check runs at the end of `--exercise`. Once the view has settled, it toggles the
shortcut sheet, the drop frame, the hold hint and the toolbar state twelve times, 50 ms apart, and
compares the engine's frame counter before and after.

## Consequences

- Chrome and model are never composited by the engine, so the chrome cannot be retinted per style in a
  shader. M5 retints it from the palette tables instead.
- Automated review needs both halves. `--snapshot` renders the engine's view offscreen and composites
  the window's layer tree over it, so a check sees what the user sees.
- The insets are fixed per layout. If a card is resized or hidden, the window reports new insets, and
  the next fit uses them. The current view does not move.
