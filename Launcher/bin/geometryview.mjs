#!/usr/bin/env node
// geometryview [file]      serves the viewer on this machine and opens it in the browser, with the
//                          file if one is given
// geometryview --mcp       runs the tool server that AI apps start (Claude, Codex, ChatGPT)
//
// The viewer is at http://127.0.0.1:47700, so the browser keeps its library from one run to the
// next. If GeometryView is already running there, the browser is opened at it. --port N, or
// GEOMETRYVIEW_PORT, chooses another port.

import { serve } from "../src/mcp.mjs";
import { openBrowser, openInPage, preferredPort, viewerServer } from "../src/page.mjs";
import { instructions, tools } from "../src/tools.mjs";
import { viewerAt } from "../src/web.mjs";

const args = process.argv.slice(2);
const at = args.indexOf("--port");
if (at >= 0) {
  process.env.GEOMETRYVIEW_PORT = args[at + 1];
  args.splice(at, 2);
}

if (args.includes("--mcp")) {
  serve(tools, instructions);
} else if (args.includes("--help") || args.includes("-h")) {
  process.stdout.write("usage: geometryview [--port N] [file] | --mcp\n");
} else {
  const file = args.find((arg) => !arg.startsWith("-"));
  const port = preferredPort();
  if (port !== 0 && (await viewerAt(port))) {
    const address = `http://127.0.0.1:${port}/`;
    process.stdout.write(`GeometryView is already running at ${address}\n`);
    if (file) {
      process.stdout.write("Open the file there, by dragging it onto the page or with ⌘O, or stop " +
                           "that viewer and run this again.\n");
    }
    await openBrowser(address);
  } else {
    const viewer = await viewerServer();
    process.stdout.write(`GeometryView is at ${viewer.address}/\nPress Ctrl-C to stop.\n`);
    if (file) {
      try {
        await openInPage(file);
      } catch (error) {
        process.stderr.write(`${error.message}\n`);
      }
    } else {
      await openBrowser(viewer.url);
    }
  }
}
