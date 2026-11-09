// The viewer in the person's browser, as an engine for the AI app's tools: on machines without the
// native engine, a model is opened, described and pictured by the page, which the person sees too.

import { execFile, spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { stat } from "node:fs/promises";
import { homedir } from "node:os";
import { basename, join, resolve } from "node:path";
import { kDefaultPort, startViewer } from "./web.mjs";

let viewer = null;
let shownFor = null; // the path the page has open, as the launcher asked

/// The port asked for: GEOMETRYVIEW_PORT, or the usual one.
export function preferredPort() {
  return Number(process.env.GEOMETRYVIEW_PORT ?? kDefaultPort);
}

/// The viewer's server, started once per launcher, on the preferred port. If something else holds
/// it, such as another launcher, on any free port: a page there has a library of its own.
export async function viewerServer() {
  if (!viewer) {
    const port = preferredPort();
    try {
      viewer = await startViewer({ port });
    } catch (error) {
      if (error.code !== "EADDRINUSE" || port === 0) throw error;
      viewer = await startViewer({ port: 0 });
      process.stderr.write(`Port ${port} is in use, so the viewer is at ${viewer.address}, where ` +
                           "the browser keeps a library of its own.\n");
    }
  }
  return viewer;
}

// Chrome on Linux gives a page WebGPU only with Vulkan on, which is off by default on NVIDIA
// GPUs at least, and flags cannot reach a Chrome already running. So on Linux the viewer opens in a
// Chrome window of its own, with its own profile, and Vulkan on.
const linuxChromes = ["/usr/bin/google-chrome", "/usr/bin/google-chrome-stable", "/usr/bin/chromium",
                      "/usr/bin/chromium-browser", "/snap/bin/chromium"];

/// The arguments that open `url` in the viewer's own Chrome window on Linux, or null without Chrome.
export function linuxViewerWindow(url, home = homedir(), exists = existsSync) {
  const chrome = process.env.GEOMETRYVIEW_CHROME ?? linuxChromes.find((path) => exists(path));
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

/// The page. If none is connected, the browser is opened at it, unless `launch` is false: then
/// the first page to connect from the viewer's address, opened anywhere, such as an AI app's own
/// browser pane.
async function page({ launch = true } = {}) {
  const server = await viewerServer();
  if (!server.bridge.connected) {
    shownFor = null;
    if (launch) await openBrowser(server.url);
  }
  return server;
}

/// The model file at `path`, checked: its full path and modification time.
async function modelFile(path) {
  const source = resolve(path);
  const info = await stat(source).catch(() => {
    throw new Error(`There is no file at ${source}.`);
  });
  if (!info.isFile()) throw new Error(`${source} is not a file.`);
  if (!/\.(step|stp|stl|geoblob)$/i.test(source)) {
    throw new Error("Only STEP (.step, .stp) and STL (.stl) files can be opened.");
  }
  return { source, info };
}

/// Opens `path` in the page, unless it is open there already. Returns the scene's description.
/// `waitMs` is how long to wait for a page to connect.
export async function openInPage(path, { launch = true, waitMs = 30_000 } = {}) {
  const { source, info } = await modelFile(path);
  const server = await page({ launch });
  if (shownFor === source && server.bridge.connected) {
    return server.bridge.request("describe", {}, { waitMs });
  }
  const description = await server.bridge.request("open", {
    url: server.grant(source), name: basename(source), lastModified: info.mtimeMs,
  }, { timeoutMs: 600_000, waitMs });
  shownFor = source;
  return description;
}

/// Opens `path` in the page and shows it as asked: `isolate`, names of parts to show alone, and a
/// `style` and `colour`, as the look card numbers them. Returns the scene's description, with
/// `isolated`, how many parts and assemblies are shown alone, when some were asked for.
export async function showInPage(path, show = {}, options = {}) {
  const description = await openInPage(path, options);
  if (show.isolate?.length > 0 || show.style !== undefined || show.colour !== undefined) {
    return (await viewerServer()).bridge.request("show", show, { waitMs: options.waitMs });
  }
  return description;
}

// How long a link waits for its page: the person may open it a while after it is offered.
const kLinkWaitMs = 30 * 60_000;

/// The viewer's address for an AI app to open in its own browser, with `path` shown in it as
/// asked: at once in a page already connected, or else in the first page to connect. Opens no
/// browser itself.
export async function linkToPage(path, show = {}) {
  await modelFile(path);
  const server = await viewerServer();
  const connected = server.bridge.connected;
  const showing = showInPage(path, show, { launch: false, waitMs: kLinkWaitMs });
  if (connected) {
    await showing;
  } else {
    showing.catch((error) => process.stderr.write(`${error.message}\n`));
  }
  return { url: server.url, shown: connected };
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
