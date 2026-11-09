// The viewer in the person's browser, as an engine for the AI app's tools: on machines without the
// native engine, a model is opened, described and pictured by the page, which the person sees too.

import { execFile, spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { stat } from "node:fs/promises";
import { homedir } from "node:os";
import { basename, join, resolve } from "node:path";
import { startViewer } from "./web.mjs";

let viewer = null;
let shownFor = null; // the path the page has open, as the launcher asked

/// The viewer's server, started once per launcher.
export async function viewerServer() {
  viewer ??= await startViewer({ port: Number(process.env.GEOMETRY_PORT ?? 0) });
  return viewer;
}

// Chrome on Linux gives a page WebGPU only with Vulkan on, which is off by default on NVIDIA
// GPUs at least, and flags cannot reach a Chrome already running. So on Linux the viewer opens in a
// Chrome window of its own, with its own profile, and Vulkan on.
const linuxChromes = ["/usr/bin/google-chrome", "/usr/bin/google-chrome-stable", "/usr/bin/chromium",
                      "/usr/bin/chromium-browser", "/snap/bin/chromium"];

/// The arguments that open `url` in the viewer's own Chrome window on Linux, or null without Chrome.
export function linuxViewerWindow(url, home = homedir(), exists = existsSync) {
  const chrome = process.env.GEOMETRY_CHROME ?? linuxChromes.find((path) => exists(path));
  if (!chrome) return null;
  const state = process.env.XDG_STATE_HOME ?? join(home, ".local/state");
  return [chrome, [`--user-data-dir=${join(state, "geometryview/chrome")}`, "--enable-features=Vulkan",
                   "--no-first-run", "--no-default-browser-check", `--app=${url}`]];
}

/// Opens `url` in the person's default browser, or on Linux the viewer's own Chrome window.
export function openBrowser(url) {
  const window = process.platform === "linux" ? linuxViewerWindow(url) : null;
  if (window) {
    // It outlives the launcher's request: the person keeps the window.
    spawn(window[0], window[1], { detached: true, stdio: "ignore" }).unref();
    return Promise.resolve();
  }
  const [command, args] =
    process.platform === "darwin" ? ["open", [url]]
    : process.platform === "win32" ? ["cmd", ["/c", "start", "", url.replace(/&/g, "^&")]]
    : ["xdg-open", [url]];
  return new Promise((done) => execFile(command, args, () => done()));
}

/// The page, opened in the browser if none is connected.
async function page() {
  const server = await viewerServer();
  if (!server.bridge.connected) {
    shownFor = null;
    await openBrowser(server.url);
  }
  return server;
}

/// Opens `path` in the page, unless it is open there already. Returns the scene's description.
export async function openInPage(path) {
  const source = resolve(path);
  const info = await stat(source).catch(() => {
    throw new Error(`There is no file at ${source}.`);
  });
  if (!info.isFile()) throw new Error(`${source} is not a file.`);
  if (!/\.(step|stp|stl|geoblob)$/i.test(source)) {
    throw new Error("Only STEP (.step, .stp) and STL (.stl) files can be opened.");
  }
  const server = await page();
  if (shownFor === source && server.bridge.connected) {
    return server.bridge.request("describe");
  }
  const description = await server.bridge.request("open", {
    url: server.grant(source), name: basename(source), lastModified: info.mtimeMs,
  }, { timeoutMs: 600_000 });
  shownFor = source;
  return description;
}

/// The open model's tree, as gcli tree gives it.
export async function pageTree(path) {
  await openInPage(path);
  return (await viewerServer()).bridge.request("structure", {});
}

/// A picture of the open model, as PNG bytes.
export async function pageRender(path, view) {
  await openInPage(path);
  const answer = await (await viewerServer()).bridge.request("render", view);
  return Buffer.from(answer.png, "base64");
}
