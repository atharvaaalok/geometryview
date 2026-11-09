// The native engine on a Mac, driven through gcli: import a STEP or STL file into a cache file,
// read its assembly tree and statistics, and render it to a PNG. The universal WebAssembly engine
// will take this module's place on other machines.

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, readFile, rename, rm, stat } from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const run = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));

/// Where gcli, the OCCT worker and the shader library are: GEOMETRYVIEW_NATIVE_DIR if set, else the
/// copy bundled beside this package, else the repository's release build.
function nativeDirectory() {
  const candidates = [
    process.env.GEOMETRYVIEW_NATIVE_DIR,
    join(here, "../native"),
    join(here, "../../build/release"),
  ].filter(Boolean);
  for (const directory of candidates) {
    if (existsSync(join(directory, "gcli")) || existsSync(join(directory, "Tools/gcli"))) {
      return directory;
    }
  }
  throw new Error("The native engine was not found. Build it with `cmake --build --preset " +
                  "release`, or set GEOMETRYVIEW_NATIVE_DIR.");
}

/// Whether this machine has the native engine (a Mac with gcli built or bundled), unless
/// GEOMETRYVIEW_ENGINE=page asks for the browser's.
export function nativeAvailable() {
  if (process.env.GEOMETRYVIEW_ENGINE === "page") return false;
  try {
    nativeDirectory();
    return process.platform === "darwin";
  } catch {
    return false;
  }
}

function binaries() {
  const directory = nativeDirectory();
  // A bundled copy is flat; a build directory keeps each target in its own folder.
  const flat = existsSync(join(directory, "gcli"));
  return {
    gcli: join(directory, flat ? "gcli" : "Tools/gcli"),
    worker: join(directory, flat ? "geometryview-occt-worker" : "Import/geometryview-occt-worker"),
    shaders: join(directory, flat ? "default.metallib" : "shaders/default.metallib"),
  };
}

const cacheDirectory = join(homedir(), "Library/Caches/geometryview");

/// Imports `path` unless an import of the same file, unchanged, is cached. Returns the cache file.
export async function importModel(path) {
  const source = resolve(path);
  const info = await stat(source).catch(() => {
    throw new Error(`There is no file at ${source}.`);
  });
  if (!info.isFile()) {
    throw new Error(`${source} is not a file.`);
  }
  if (!/\.(step|stp|stl)$/i.test(source)) {
    throw new Error("Only STEP (.step, .stp) and STL (.stl) files can be opened.");
  }
  const key = createHash("sha256")
    .update(`${source}\0${info.size}\0${info.mtimeMs}`)
    .digest("hex")
    .slice(0, 32);
  const blob = join(cacheDirectory, `${key}.geoblob`);
  if (existsSync(blob)) {
    return { blob, source, fromCache: true };
  }
  await mkdir(cacheDirectory, { recursive: true });
  const { gcli, worker } = binaries();
  const partial = `${blob}.${process.pid}.partial`;
  try {
    await run(gcli, ["import", source, partial, worker], { maxBuffer: 1 << 20 });
  } catch (error) {
    await rm(partial, { force: true });
    throw new Error(`Could not import ${basename(source)}: ${(error.stderr || error.message).trim()}`);
  }
  await rename(partial, blob);
  return { blob, source, fromCache: false };
}

/// The assembly tree: one node per entry, depth-first, with parent, kind, name and body range.
export async function readTree(blob) {
  const { gcli } = binaries();
  const { stdout } = await run(gcli, ["tree", blob], { maxBuffer: 1 << 30 });
  return JSON.parse(stdout);
}

/// Faces, triangles, area, signed volume and the bounding box.
export async function readStats(blob) {
  const { gcli } = binaries();
  const { stdout } = await run(gcli, ["stats", blob], { maxBuffer: 1 << 20 });
  return JSON.parse(stdout);
}

/// Renders the cache file and returns the PNG's bytes.
export async function render(blob, view) {
  const { gcli, shaders } = binaries();
  const png = join(tmpdir(), `geometry-view-${process.pid}-${Date.now()}.png`);
  const args = [
    "render", shaders, blob, png,
    String(view.width), String(view.height),
    String(view.azimuth), String(view.elevation),
    view.perspective ? "perspective" : "ortho",
    String(view.style), String(view.colour),
    view.section ?? "",
    view.overlay ? "overlay" : "plain",
    (view.isolate ?? []).join(","),
  ];
  try {
    await run(gcli, args, { maxBuffer: 1 << 20 });
    return await readFile(png);
  } catch (error) {
    throw new Error((error.stderr || error.message).trim());
  } finally {
    await rm(png, { force: true });
  }
}

/// The native GeometryView app, if it is installed or built.
export function viewerApplication() {
  const candidates = [
    process.env.GEOMETRYVIEW_APP,
    "/Applications/GeometryView.app",
    join(homedir(), "Applications/GeometryView.app"),
    join(here, "../../build/release/App/GeometryView.app"),
  ].filter(Boolean);
  return candidates.find((candidate) => existsSync(candidate)) ?? null;
}

/// Opens `path` in the app, then, if asked, shows some parts alone, in a style and colours: a
/// geometryview://show request sent after the file, which the app applies once the model is in.
/// The file itself goes the usual way, which grants the app's sandbox access to it.
export async function openInViewer(path, { isolate = [], style, colour } = {}) {
  const application = viewerApplication();
  if (!application) {
    throw new Error("The GeometryView app was not found on this Mac.");
  }
  const file = resolve(path);
  await run("open", ["-a", application, file]);
  if (isolate.length > 0 || style || colour) {
    const query = new URLSearchParams({ file });
    for (const name of isolate) query.append("isolate", name);
    if (style) query.set("style", style);
    if (colour) query.set("colour", colour);
    await run("open", ["-a", application, `geometryview://show?${query}`]);
  }
  return application;
}
