#!/usr/bin/env node
// Runs one of the viewer's scripted checks in headless Chrome and prints its report: starts the
// spike server (Web/spike/serve.mjs), opens the page, waits for the report the page posts, and
// closes both.
//
//   node Tools/web_report.mjs "<path and query under the server>" [timeout seconds]
//   node Tools/web_report.mjs "/viewer/?test=thumbs&label=mac-chrome&count=60"
//
// A page that asks for a screenshot (GET /shot?name=...) gets one, through Chrome's DevTools,
// written to build/fixtures/out/<name>.png.
//
// Chrome is GEOMETRYVIEW_CHROME, or Google Chrome's usual place on a Mac. Its window is 1440 × 900,
// or GEOMETRYVIEW_WINDOW ("390,844" for a phone's). Chrome makes no window narrower than 500, so a
// narrower one is a phone's screen emulated through DevTools: 3 pixels a point, touch, mobile.
// GEOMETRYVIEW_SCALE ("2") draws a desktop window at that many pixels a point, as a Retina screen
// does, so its screenshots are sharp.

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
const devtools = port + 1000;
const server = spawn(process.execPath, [join(root, "Web/spike/serve.mjs"), String(port)],
                     { stdio: ["ignore", "pipe", "inherit"] });
const profile = mkdtempSync(join(tmpdir(), "geometryview-report-"));
let chrome = null;

const finish = (code) => {
  emulation?.close();
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
                                `--remote-debugging-port=${devtools}`,
                                "--enable-unsafe-webgpu", `--window-size=${Math.max(500, screenSize[0])},${screenSize[1]}`,
                                ...(scale && !emulated ? [`--force-device-scale-factor=${scale}`] : []),
                                emulated ? "about:blank" : `http://127.0.0.1:${port}${page}`], { stdio: "ignore" });
    if (emulated) {
      emulate(`http://127.0.0.1:${port}${page}`).catch((error) => {
        process.stderr.write(`emulation failed: ${error.message}\n`);
        finish(1);
      });
    }
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
    } else if (line.startsWith("shot: ")) {
      screenshot(line.slice(6).trim()).catch((error) => process.stderr.write(`shot failed: ${error.message}\n`));
    }
  }
});

const screenSize = (process.env.GEOMETRYVIEW_WINDOW ?? "1440,900").split(",").map(Number);
const emulated = screenSize[0] < 500;
const scale = Number(process.env.GEOMETRYVIEW_SCALE ?? 0);
let emulation = null; // the DevTools session holding the emulation, kept open for the run

// Opens the page on an emulated phone screen.
async function emulate(url) {
  let target = null;
  for (let i = 0; i < 100 && !target; i++) {
    try {
      target = (await (await fetch(`http://127.0.0.1:${devtools}/json/list`)).json()).find((p) => p.type === "page");
    } catch {
      // Chrome is still starting.
    }
    if (!target) await new Promise((resolve) => setTimeout(resolve, 100));
  }
  if (!target) throw new Error("no page");
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });
  let id = 0;
  const send = (method, params) => new Promise((resolve) => {
    const mine = ++id;
    const listen = ({ data }) => {
      if (JSON.parse(data).id !== mine) return;
      socket.removeEventListener("message", listen);
      resolve();
    };
    socket.addEventListener("message", listen);
    socket.send(JSON.stringify({ id: mine, method, params }));
  });
  await send("Emulation.setDeviceMetricsOverride",
             { width: screenSize[0], height: screenSize[1], deviceScaleFactor: 3, mobile: true });
  await send("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
  await send("Page.navigate", { url });
  emulation = socket;
}

// A screenshot of the page, through Chrome's DevTools protocol.
async function screenshot(name) {
  const pages = await (await fetch(`http://127.0.0.1:${devtools}/json/list`)).json();
  const page = pages.find((p) => p.type === "page");
  if (!page) throw new Error("no page");
  const socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.onopen = resolve;
    socket.onerror = reject;
  });
  const shot = await new Promise((resolve) => {
    socket.onmessage = ({ data }) => {
      const message = JSON.parse(data);
      if (message.id === 1) resolve(message.result?.data);
    };
    socket.send(JSON.stringify({ id: 1, method: "Page.captureScreenshot", params: { format: "png" } }));
  });
  socket.close();
  const { mkdirSync, writeFileSync } = await import("node:fs");
  const folder = join(root, "build/fixtures/out");
  mkdirSync(folder, { recursive: true });
  const file = join(folder, `${name}.png`);
  writeFileSync(file, Buffer.from(shot, "base64"));
  process.stdout.write(`shot: ${file}\n`);
}
