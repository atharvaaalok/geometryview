#!/usr/bin/env node
// geometry-viewer --mcp   runs the tool server that AI apps start (Claude, Codex, ChatGPT).

import { serve } from "../src/mcp.mjs";
import { instructions, tools } from "../src/tools.mjs";

if (process.argv.includes("--mcp")) {
  serve(tools, instructions);
} else {
  process.stderr.write("usage: geometry-viewer --mcp\n");
  process.exit(2);
}
