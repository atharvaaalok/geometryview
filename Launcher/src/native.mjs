// The native engine on a Mac, driven through gcli: import a STEP or STL file into a cache file,
// read its assembly tree and statistics, and render it to a PNG. The universal WebAssembly engine
// will take this module's place on other machines.

import { execFile, spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, open, readdir, readFile, rename, rm, stat, utimes } from "node:fs/promises";
import { cpus, homedir, release, tmpdir } from "node:os";
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

/// Whether this machine can run the native engine, which needs Metal 4: Apple silicon and macOS 26
/// or later, whose kernel is Darwin 25.
export function canRunNative(platform = process.platform, arch = process.arch, kernel = release()) {
  return platform === "darwin" && arch === "arm64" && Number(kernel.split(".")[0]) >= 25;
}

/// Whether this machine has the native engine (gcli built or bundled) and can run it, unless
/// GEOMETRYVIEW_ENGINE=page asks for the browser's. Other machines, older Macs with the Mac
/// extension among them, use the browser's.
export function nativeAvailable() {
  if (process.env.GEOMETRYVIEW_ENGINE === "page" || !canRunNative()) return false;
  try {
    nativeDirectory();
    return true;
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

// Imports and renders at once: a third of the cores, at most four, so a folder's worth of files
// leaves the machine usable (ADR 0016).
const slots = Math.max(1, Math.min(4, Math.floor(cpus().length / 3)));
let busy = 0;
const queued = [];

/// Runs `work` once a slot is free.
export async function withSlot(work) {
  if (busy >= slots) await new Promise((resolve) => queued.push(resolve));
  busy += 1;
  try {
    return await work();
  } finally {
    busy -= 1;
    queued.shift()?.();
  }
}

// Imports under way, by cache file, so the same file asked for twice is imported once.
const importing = new Map();

// The cache's budget: GEOMETRYVIEW_CACHE_BYTES, else 4 GB. Past it, the files used longest ago go,
// down to 80% of it, in a sweep run 30 s after the last import; a cache file's time is its last use.
// Partial files left by an import that died go once they are an hour old.
const kCacheBudget = Number(process.env.GEOMETRYVIEW_CACHE_BYTES) || 4 * 1024 ** 3;
let sweepTimer = null;

function scheduleSweep() {
  clearTimeout(sweepTimer);
  sweepTimer = setTimeout(() => sweepCache().catch(() => {}), 30_000);
  sweepTimer.unref?.();
}

/// Keeps the cache within `budget` bytes, sparing files used in the last minute. Returns the bytes
/// it holds after.
export async function sweepCache(directory = cacheDirectory, budget = kCacheBudget, now = Date.now()) {
  const names = await readdir(directory).catch(() => []);
  const files = [];
  for (const name of names) {
    const path = join(directory, name);
    const info = await stat(path).catch(() => null);
    if (!info?.isFile()) continue;
    if (name.endsWith(".partial")) {
      if (now - info.mtimeMs > 3600_000) await rm(path, { force: true });
      continue;
    }
    if (name.endsWith(".geoblob")) files.push({ path, bytes: info.size, used: info.mtimeMs });
  }
  let total = files.reduce((sum, file) => sum + file.bytes, 0);
  if (total <= budget) return total;
  files.sort((a, b) => a.used - b.used);
  for (const file of files) {
    if (total <= budget * 0.8) break;
    if (now - file.used < 60_000) continue;
    await rm(file.path, { force: true });
    total -= file.bytes;
  }
  return total;
}

/// Imports `path` unless an import of the same file, unchanged, is cached. Returns the cache file.
// The cache file version gcli writes and reads, from Import/format/blob.h.
export const kCacheVersion = 5;

// Whether a cache file is in the current format: 8 bytes of magic, then a little-endian u32.
async function isCurrent(path) {
  const file = await open(path, "r");
  try {
    const header = Buffer.alloc(12);
    const { bytesRead } = await file.read(header, 0, 12, 0);
    return bytesRead === 12 && header.readUInt32LE(8) === kCacheVersion;
  } finally {
    await file.close();
  }
}

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
    // One in an older format is made again: gcli reads only its own. Its time marks its use.
    if (await isCurrent(blob)) {
      const now = new Date();
      await utimes(blob, now, now).catch(() => {});
      return { blob, source, fromCache: true };
    }
    await rm(blob, { force: true });
  }
  if (!importing.has(blob)) {
    const work = withSlot(async () => {
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
      scheduleSweep();
    }).finally(() => importing.delete(blob));
    importing.set(blob, work);
  }
  await importing.get(blob);
  return { blob, source, fromCache: false };
}

/// The STEP and STL files in `folder`, and with `recursive` in its folders too, hidden ones left
/// out: at most 20,000, 32 folders deep.
export async function listModels(folder, { recursive = false } = {}) {
  const root = resolve(folder);
  const info = await stat(root).catch(() => {
    throw new Error(`There is no folder at ${root}.`);
  });
  if (!info.isDirectory()) throw new Error(`${root} is not a folder.`);
  const found = [];
  const walk = async (directory, depth) => {
    const entries = await readdir(directory, { withFileTypes: true }).catch(() => []);
    for (const entry of entries) {
      if (found.length >= 20000 || entry.name.startsWith(".")) continue;
      const path = join(directory, entry.name);
      if (entry.isDirectory()) {
        if (recursive && depth < 32) await walk(path, depth + 1);
      } else if (entry.isFile() && /\.(step|stp|stl)$/i.test(entry.name)) {
        found.push(path);
      }
    }
  };
  await walk(root, 0);
  return found;
}

/// A cache file's facts: its box in millimetres, parts, bodies, faces, triangles and the name
/// inside it (scene::Facts).
export async function readFacts(blob) {
  const { gcli } = binaries();
  const { stdout } = await run(gcli, ["facts", blob], { maxBuffer: 1 << 20 });
  return JSON.parse(stdout);
}

/// Draws cache files on one contact sheet, as the gallery draws them: `tiles` are { blob, label,
/// sub }. Returns the PNG's bytes and, for each tile, its cell and any error.
export async function contactSheet(tiles, { cell = 192, columns, style = "illustration", colour = "parts",
                                            sameScale = false, title } = {}) {
  const { gcli, shaders } = binaries();
  const png = join(tmpdir(), `geometry-view-sheet-${process.pid}-${Date.now()}.png`);
  const args = ["sheet", shaders, png, "--cell", String(cell), "--style", String(style),
                "--colour", String(colour)];
  if (columns) args.push("--columns", String(columns));
  if (sameScale) args.push("--same-scale");
  if (title) args.push("--title", title);
  try {
    const stdout = await withSlot(() => new Promise((resolve, reject) => {
      const child = spawn(gcli, args, { stdio: ["pipe", "pipe", "pipe"] });
      let out = "", err = "";
      child.stdout.on("data", (chunk) => { out += chunk; });
      child.stderr.on("data", (chunk) => { err += chunk; });
      child.on("error", reject);
      child.on("close", (code) => (code === 0 ? resolve(out) : reject(new Error(err.trim() || `gcli sheet failed (${code})`))));
      child.stdin.end(tiles.map((tile) => JSON.stringify(tile)).join("\n") + "\n");
    }));
    const placed = stdout.split("\n").filter(Boolean).map((line) => JSON.parse(line));
    return { png: await readFile(png), tiles: placed };
  } finally {
    await rm(png, { force: true });
  }
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
    await withSlot(() => run(gcli, args, { maxBuffer: 1 << 20 }));
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
