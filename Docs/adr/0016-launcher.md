# ADR 0016: The launcher: the viewer on any computer, and AI apps through it

Status: accepted (2026-11-07). P5 of ADR 0013.

## Context

S1's launcher gave AI apps four tools: `open_model`, `get_structure`, `render_view` and
`open_in_viewer`. They were backed by `gcli` and the Geometry app, so they worked only on a Mac.

P4 made a viewer that runs in any browser. P5 had three aims:
- serve that viewer from the same launcher;
- bridge it to the AI app;
- package it so Claude, Codex and ChatGPT install it on Windows and Linux as well as the Mac.

## Decisions

**1. One Node launcher, no dependencies, two modes.**
- `geometry-viewer` serves the viewer on 127.0.0.1 and opens it in the default browser.
- `geometry-viewer --mcp` is the tool server an AI app starts. It starts the viewer's server the
  first time a tool needs it.
- Node is all it needs: Claude ships it, and `npx` brings it to Codex and ChatGPT.

**2. The server trusts only this machine, this run and the page it served.**
- It listens on 127.0.0.1 and answers only requests that name it by its loopback address. That
  shuts out DNS rebinding.
- Each run makes a random token, passed to the page in its URL.
- A model file is served only when the AI app asked to show it, under a random path, with the
  token. Static files cannot leave the viewer's folders.
- The bridge is a WebSocket. It takes only the page's own origin with the token, so another site
  in the same browser can neither read a file nor drive the viewer.
- Every response carries the cross-origin isolation headers, which the importer's threads need.

**3. The bridge is plain JSON over a small WebSocket written here.**
- Requests carry `{ id, method, params }`; answers carry `{ id, result }` or `{ id, error }`.
- `Launcher/src/websocket.mjs` implements just what the bridge needs: the handshake, masked text
  frames from the page, ping and close.
- The page reconnects if the launcher restarts.

**4. Without a native engine, the tools work through the page, which the person sees.**
- On a Mac with `gcli` built or bundled, the tools keep S1's native path. Pictures need no browser
  there, and `open_in_viewer` opens the Geometry app.
- Anywhere else, or with `GEOMETRYVIEW_ENGINE=page`, the launcher opens the viewer in the browser and
  asks it to:
  - import or open the file, and describe it;
  - list its tree;
  - draw the picture asked for: the direction, the style and colours, parts shown alone, the cuts
    and the size.
- The viewer keeps showing what was asked for, so the person sees what the AI looked at.
- A picture is the view as drawn:
  - cropped to the part the cards leave clear;
  - letterboxed with its own background;
  - drawn through a snapshot that does not wait for the frame loop, which browsers pause for
    hidden pages.

**5. Packaging carries the viewer everywhere, and the native engine where it exists.**
- `scripts/web.mjs` gathers the page, its engine, its importers and fonts into `Launcher/web`.
- That folder goes into:
  - the npm package: 4.5 MB, run with `npx` from a packed tarball;
  - the `.mcpb` bundle, which now declares macOS, Windows and Linux.
- The bundle adds the native engine where it was built.
- In the repository, the launcher serves the sources and builds directly, so an edit shows at once.

## Consequences

- **Claude, Codex and ChatGPT can open, outline and picture models on any computer with a
  browser that has WebGPU.**
  - On this Mac, through the browser: `open_model` took 1.1 s on PLE120, opening the browser and
    importing; `render_view` 48–270 ms.
  - The 10M STL took 3.2 s in headless Chrome.
- **A Windows or Linux run** is still to be done (P7).
- **The AI's pictures change what the person sees** (look, cuts, parts shown alone). That is
  intended, but a person in the middle of their own work may not want it. A separate,
  invisible view for the AI would need a second set of render targets; it waits for a need.
- **The end-to-end test** (`GEOMETRYVIEW_E2E=1`) needs Chrome and a GPU, so it is not part of the
  default suite.
- **Found along the way:** Metal allows only a few timing counter buffers across the whole system.
  With dozens of viewer tabs open, the renderer's query set could not be made, and every frame
  failed with it. The renderer now adopts the query set only once the GPU confirms it, and goes
  untimed otherwise.
