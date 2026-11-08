#!/usr/bin/env node
// geometry-viewer          serves the viewer on this machine and opens it in the browser
// geometry-viewer --mcp    runs the tool server that AI apps start (Claude, Codex, ChatGPT)
//
// --port N sets the viewer's port (default: any free one); GEOMETRY_PORT does too.

import { serve } from "../src/mcp.mjs";
import { openBrowser, viewerServer } from "../src/page.mjs";
import { instructions, tools } from "../src/tools.mjs";

const at = process.argv.indexOf("--port");
if (at >= 0) process.env.GEOMETRY_PORT = process.argv[at + 1];

if (process.argv.includes("--mcp")) {
  serve(tools, instructions);
} else if (process.argv.includes("--help")) {
  process.stdout.write("usage: geometry-viewer [--port N] | --mcp\n");
} else {
  const viewer = await viewerServer();
  process.stdout.write(`Geometry is at ${viewer.url}\nPress Ctrl-C to stop.\n`);
  await openBrowser(viewer.url);
}
