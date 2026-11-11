#!/usr/bin/env node
// Runs one of the viewer's scripted checks in headless Chrome and prints its report: starts the
// spike server (Web/spike/serve.mjs), opens the page, waits for the report the page posts, and
// closes both.
//
//   node Tools/web_report.mjs "<path and query under the server>" [timeout seconds]
//   node Tools/web_report.mjs "/viewer/?test=thumbs&label=mac-chrome&count=60"
//
// Chrome is GEOMETRYVIEW_CHROME, or Google Chrome's usual place on a Mac.

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const [page, timeout = "180"] = process.argv.slice(2);
if (!page) {
  process.stderr.write("usage: web_report.mjs <path and query> [timeout seconds]\n");
  process.exit(2);
}
const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const port = 7400 + Math.floor(Math.random() * 400);
const server = spawn(process.execPath, [join(root, "Web/spike/serve.mjs"), String(port)],
                     { stdio: ["ignore", "pipe", "inherit"] });
const profile = mkdtempSync(join(tmpdir(), "geometryview-report-"));
let chrome = null;

const finish = (code) => {
  chrome?.kill("SIGKILL");
  server.kill();
  rmSync(profile, { recursive: true, force: true });
  process.exit(code);
};
setTimeout(() => {
  process.stderr.write("timed out\n");
  finish(1);
}, Number(timeout) * 1000);

server.stdout.setEncoding("utf8");
let buffered = "";
server.stdout.on("data", (text) => {
  buffered += text;
  if (!chrome && buffered.length > 0) {
    // The server prints once it listens; then the page opens.
    const chromePath = process.env.GEOMETRYVIEW_CHROME ?? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
    chrome = spawn(chromePath, ["--headless=new", `--user-data-dir=${profile}`, "--no-first-run",
                                "--enable-unsafe-webgpu", "--window-size=1440,900",
                                `http://127.0.0.1:${port}${page}`], { stdio: "ignore" });
  }
  for (const line of text.split("\n")) {
    const match = /^report: (.+\.json)$/.exec(line.trim());
    if (match) {
      import("node:fs").then(({ readFileSync }) => {
        process.stdout.write(`${match[1]}\n${readFileSync(match[1], "utf8")}`);
        finish(0);
      });
    } else if (line.startsWith("png: ")) {
      process.stdout.write(`${line.trim()}\n`);
    }
  }
});
