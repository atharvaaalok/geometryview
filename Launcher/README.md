# GeometryView

**See inside your CAD models, fast.** Open STEP and STL files in your browser, and let your AI apps
open, inspect and picture them.

<img src="https://raw.githubusercontent.com/atharvaaalok/geometryview/main/Docs/media/styles.jpg" alt="A drive cut open, in six drawing styles" width="600">

## View a model

```sh
npx geometryview part.step     # opens it in your browser
npx geometryview               # the viewer, with your folders and recent files
```

The viewer is served from your computer only (127.0.0.1), and your files never leave it. It needs a
browser with WebGPU: Chrome or Edge, or Safari 26 or later. Or use the hosted copy, with sample
models: [atharvaaalok.github.io/geometryview](https://atharvaaalok.github.io/geometryview/).

## Use it from your AI app

`geometryview --mcp` is a tool server (MCP) with five tools:

- `open_model` opens a file and sums it up.
- `get_structure` lists its assemblies and parts.
- `render_view` returns a picture: any view, any of six styles, sections, parts alone.
- `view_folder` pictures a whole folder on one contact sheet, with a numbered index of sizes and
  parts. It needs the native engine (a Mac with Apple silicon and macOS 26).
- `open_in_viewer` shows it to you, live.

```sh
claude mcp add geometryview -- npx -y geometryview --mcp     # Claude Code
codex mcp add geometryview -- npx -y geometryview --mcp      # Codex
```

Other apps take the same command, `npx -y geometryview --mcp`, in their MCP settings. Claude
Desktop can also install it in one click from the `.mcpb` in the
[latest release](https://github.com/atharvaaalok/geometryview/releases/latest).

More, including the Mac app and the iPhone: [github.com/atharvaaalok/geometryview](https://github.com/atharvaaalok/geometryview).
MIT licensed.
