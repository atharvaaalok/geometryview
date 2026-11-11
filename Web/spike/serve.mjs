// Serves the renderer and import spikes on this machine only: the pages, the WebAssembly builds
// and the fixtures, with cross-origin isolation, and saves each run's report.
//
// Usage: node Web/spike/serve.mjs [port]
// Open http://127.0.0.1:<port>/?model=stl10m&label=<where it runs>[&w=2880&h=1800&copies=1]
// or http://127.0.0.1:<port>/import.html?model=ple120&workers=4&label=<where it runs>
// or http://127.0.0.1:<port>/step.html?model=hard&workers=12&label=<where it runs>
// or http://127.0.0.1:<port>/renderer.html?model=stl10m&label=<where it runs>

import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { mkdir, readdir, rm, stat, writeFile } from "node:fs/promises";
import { dirname, extname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "../..");
// Reports go beside the spike they came from; whole STEP imports are P2's, the viewer's own
// renderer P3's.
const reportsFor = (label) => {
  // The STEP sweep's report stays beside the cache files it names, for Tools/web_step_sweep.py.
  if (label.includes("-p7-sweep")) return join(root, "build/fixtures/out");
  if (label.includes("-p4-")) return join(root, "Docs/measurements/p4/runtime");
  if (label.includes("-import-step-")) return join(root, "Docs/measurements/p2/step");
  // The gallery's pictures, drawn as the browser will draw them, and its other checks.
  if (label.includes("-thumbs-") || label.includes("-g1-") || label.includes("-g2-")) {
    return join(root, "Docs/measurements/gallery/web");
  }
  if (label.includes("-p3-")) return join(root, "Docs/measurements/p3/renderer");
  return join(root, "Docs/measurements/phase0", label.includes("-import-") ? "import" : "renderer");
};
const port = Number(process.argv[2] ?? 7342);
const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
                ".wasm": "application/wasm", ".json": "application/json", ".css": "text/css",
                ".ttf": "font/ttf", ".svg": "image/svg+xml" };
// What each URL prefix serves.
const mounts = [
  // The universal viewer's page, its engine and fonts, and the fixtures it opens.
  ["/viewer/build/", join(root, "build/wasm-webgpu/Engine")],
  ["/viewer/fonts/", join(root, "ThirdParty/ibm-plex-mono")],
  ["/viewer/fixtures/", join(root, "build/fixtures")],
  // GeometryView's own test models, for the gallery's checks, and any other folders named in
  // GEOMETRYVIEW_GALLERY_FOLDERS as name=path,name=path, read only.
  ["/viewer/test-models/", join(root, "test-models")],
  ...(process.env.GEOMETRYVIEW_GALLERY_FOLDERS ?? "").split(",").filter((pair) => pair.includes("="))
    .map((pair) => [`/viewer/${pair.split("=")[0]}/`, pair.slice(pair.indexOf("=") + 1)]),
  ["/viewer/occt/", join(root, "build/wasm-release/Import")],
  ["/viewer/occt-threads/", join(root, "build/wasm-threads/Import")],
  ["/viewer/", join(root, "Web/viewer")],
  ["/build/", join(root, "build/wasm-webgpu/Tools/spikes/webgpu")],
  ["/occt/", join(root, "build/wasm-release/Import")],
  ["/bench/", join(root, "build/wasm-webgpu/Tools")],
  ["/fixtures/", join(root, "build/fixtures")],
  ["/", here],
];

const server = createServer(async (request, response) => {
  const headers = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Cache-Control": "no-store",
  };
  const url = new URL(request.url, `http://${request.headers.host}`);
  // A served folder's STEP and STL files and its folders, for the gallery's checks:
  // /list?dir=test-models/models answers [{ path, folder, bytes, modified }].
  if (request.method === "GET" && url.pathname === "/list") {
    const dir = String(url.searchParams.get("dir") ?? "");
    const mount = mounts.find(([prefix]) => `/viewer/${dir}/`.startsWith(prefix) && prefix !== "/viewer/");
    if (!mount || dir.includes("..")) {
      response.writeHead(404, headers).end();
      return;
    }
    const base = join(mount[1], `/viewer/${dir}/`.slice(mount[0].length));
    const entries = [];
    const walk = async (folder, prefix) => {
      for (const entry of await readdir(folder, { withFileTypes: true })) {
        if (entry.name.startsWith(".")) continue;
        const path = prefix ? `${prefix}/${entry.name}` : entry.name;
        if (entry.isDirectory()) {
          entries.push({ path, folder: true });
          await walk(join(folder, entry.name), path);
        } else if (/\.(step|stp|stl)$/i.test(entry.name)) {
          const info = await stat(join(folder, entry.name));
          entries.push({ path, folder: false, bytes: info.size, modified: Math.round(info.mtimeMs) });
        }
      }
    };
    await walk(base, "");
    response.writeHead(200, { ...headers, "Content-Type": "application/json" }).end(JSON.stringify(entries));
    return;
  }
  // A page asks for a screenshot of itself: whoever runs the browser takes it (Tools/web_report.mjs).
  if (request.method === "GET" && url.pathname === "/shot") {
    console.log(`shot: ${String(url.searchParams.get("name") ?? "shot").replace(/[^\w.-]+/g, "-")}`);
    response.writeHead(204, headers).end();
    return;
  }
  if (request.method === "POST" && url.pathname === "/report") {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const report = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    const label = String(report.label ?? "unlabelled").replace(/[^\w.-]+/g, "-");
    const reports = reportsFor(label);
    await mkdir(reports, { recursive: true });
    const file = join(reports, `${label}${report.partial ? ".partial" : ""}.json`);
    await writeFile(file, JSON.stringify(report, null, 2) + "\n");
    if (!report.partial) {
      await rm(join(reports, `${label}.partial.json`), { force: true });
    }
    console.log(`${report.partial ? "partial" : "report"}: ${file}`);
    response.writeHead(204, headers).end();
    return;
  }
  // A picture of a page, as PNG bytes, kept to look at.
  if (request.method === "POST" && url.pathname === "/png") {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const name = String(url.searchParams.get("label") ?? "unlabelled").replace(/[^\w.-]+/g, "-");
    const folder = join(root, "build/fixtures/out");
    await mkdir(folder, { recursive: true });
    const file = join(folder, `${name}.png`);
    await writeFile(file, Buffer.concat(chunks));
    console.log(`png: ${file}`);
    response.writeHead(204, headers).end();
    return;
  }
  // A cache file an import made, kept so the native tools can check it.
  if (request.method === "POST" && url.pathname === "/blob") {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const name = String(url.searchParams.get("label") ?? "unlabelled").replace(/[^\w.-]+/g, "-");
    const folder = join(root, "build/fixtures/out");
    await mkdir(folder, { recursive: true });
    const file = join(folder, `${name}.geoblob`);
    await writeFile(file, Buffer.concat(chunks));
    console.log(`blob: ${file}`);
    response.writeHead(204, headers).end();
    return;
  }
  const path = url.pathname === "/" ? "/index.html" : url.pathname;
  const [prefix, directory] = mounts.find(([p]) => path.startsWith(p));
  const name = path.slice(prefix.length) || "index.html";
  if (name.includes("..")) {
    response.writeHead(400, headers).end();
    return;
  }
  const file = join(directory, name);
  try {
    const info = await stat(file);
    response.writeHead(200, { ...headers, "Content-Length": info.size,
                              "Content-Type": types[extname(name)] ?? "application/octet-stream" });
    createReadStream(file).pipe(response);
  } catch {
    response.writeHead(404, headers).end();
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`spike at http://127.0.0.1:${port}/?model=stl10m&label=<where>`);
});
