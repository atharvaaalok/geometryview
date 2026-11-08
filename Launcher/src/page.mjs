// The viewer in the person's browser, as an engine for the AI app's tools: on machines without the
// native engine, a model is opened, described and pictured by the page, which the person sees too.

import { execFile } from "node:child_process";
import { stat } from "node:fs/promises";
import { basename, resolve } from "node:path";
import { startViewer } from "./web.mjs";

let viewer = null;
let shownFor = null; // the path the page has open, as the launcher asked

/// The viewer's server, started once per launcher.
export async function viewerServer() {
  viewer ??= await startViewer({ port: Number(process.env.GEOMETRY_PORT ?? 0) });
  return viewer;
}

/// Opens `url` in the person's default browser.
export function openBrowser(url) {
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
