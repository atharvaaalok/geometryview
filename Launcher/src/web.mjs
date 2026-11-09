// The viewer, served to this machine only: the page and its engine with cross-origin isolation, the
// model files the AI app asked to show, and the bridge to the page.
//
// Every request must name this server by its loopback address, which shuts out DNS rebinding;
// model files and the bridge need the token this run made, and the bridge the page's own origin,
// so another site in the same browser can neither read a file nor drive the viewer.

import { randomBytes } from "node:crypto";
import { createReadStream, existsSync } from "node:fs";
import { stat } from "node:fs/promises";
import { createServer } from "node:http";
import { basename, dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { Bridge } from "./bridge.mjs";
import { acceptWebSocket } from "./websocket.mjs";

const here = dirname(fileURLToPath(import.meta.url));

const kTypes = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript",
  ".css": "text/css", ".wasm": "application/wasm", ".json": "application/json",
  ".ttf": "font/ttf", ".svg": "image/svg+xml", ".png": "image/png",
};

/// Where the viewer's files are, as URL prefixes and folders: GEOMETRYVIEW_WEB_DIR; else, in the
/// repository, its sources and builds, so an edit shows at once; else the copy packed beside this
/// package.
export function viewerMounts() {
  const root = join(here, "../..");
  const packed = [process.env.GEOMETRYVIEW_WEB_DIR,
                  existsSync(join(root, "Web/viewer/index.html")) ? null : join(here, "../web")];
  for (const directory of packed.filter(Boolean)) {
    if (existsSync(join(directory, "index.html"))) return [["/", directory]];
  }
  return [
    ["/build/", join(root, "build/wasm-webgpu/Engine")],
    ["/fonts/", join(root, "ThirdParty/ibm-plex-mono")],
    ["/occt/", join(root, "build/wasm-release/Import")],
    ["/occt-threads/", join(root, "build/wasm-threads/Import")],
    ["/", join(root, "Web/viewer")],
  ];
}

/// Starts the server on 127.0.0.1. Returns { url, port, token, bridge, grant(path), close() }.
export async function startViewer({ port = 0 } = {}) {
  const token = randomBytes(16).toString("hex");
  const mounts = viewerMounts();
  const grants = new Map(); // id to an absolute path the AI app asked to show
  const bridge = new Bridge();
  const headers = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "X-Content-Type-Options": "nosniff",
    "Cache-Control": "no-cache",
  };
  let address = "";
  // Bodies go chunked, with their size apart. Safari, like any WebKit, holds back a body whose
  // length it knows and hands it to the page some 80 ms at a time, so a cache file meant to stream
  // into GPU buffers arrived in pieces of up to its whole size, late. A HEAD answer has no body to
  // hold back.
  const sized = (request, size) =>
    request.method === "HEAD" ? { "Content-Length": size } : { "GeometryView-Size": size };
  const ours = (request) => {
    const host = request.headers.host ?? "";
    return host === `127.0.0.1:${server.address().port}` || host === `localhost:${server.address().port}`;
  };

  const server = createServer(async (request, response) => {
    if (!ours(request)) {
      response.writeHead(421).end();
      return;
    }
    const url = new URL(request.url, address);
    if (request.method !== "GET" && request.method !== "HEAD") {
      response.writeHead(405, headers).end();
      return;
    }
    // A model file the AI app asked to show.
    if (url.pathname.startsWith("/file/")) {
      const path = grants.get(url.pathname.slice("/file/".length));
      if (url.searchParams.get("token") !== token || !path) {
        response.writeHead(403, headers).end();
        return;
      }
      try {
        const info = await stat(path);
        response.writeHead(200, { ...headers, ...sized(request, info.size),
                                  "Content-Type": "application/octet-stream",
                                  "Last-Modified": info.mtime.toUTCString() });
        if (request.method === "HEAD") response.end();
        else createReadStream(path).pipe(response);
      } catch {
        response.writeHead(404, headers).end();
      }
      return;
    }
    const path = url.pathname === "/" ? "/index.html" : url.pathname;
    const [prefix, directory] = mounts.find(([p]) => path.startsWith(p));
    const file = resolve(directory, decodeURIComponent(path.slice(prefix.length)));
    if (!file.startsWith(resolve(directory) + sep)) {
      response.writeHead(400, headers).end();
      return;
    }
    try {
      const info = await stat(file);
      if (!info.isFile()) throw new Error("not a file");
      response.writeHead(200, { ...headers, ...sized(request, info.size),
                                "Content-Type": kTypes[extname(file)] ?? "application/octet-stream" });
      if (request.method === "HEAD") response.end();
      else createReadStream(file).pipe(response);
    } catch {
      response.writeHead(404, headers).end();
    }
  });

  server.on("upgrade", (request, socket) => {
    const url = new URL(request.url, address || "http://127.0.0.1");
    if (!ours(request) || url.pathname !== "/bridge" || url.searchParams.get("token") !== token ||
        request.headers.origin !== address) {
      socket.end("HTTP/1.1 403 Forbidden\r\n\r\n");
      return;
    }
    const connection = acceptWebSocket(request, socket);
    if (connection) bridge.attach(connection);
  });

  await new Promise((done, fail) => {
    server.once("error", fail);
    server.listen(port, "127.0.0.1", done);
  });
  address = `http://127.0.0.1:${server.address().port}`;
  return {
    url: `${address}/?token=${token}`,
    address,
    port: server.address().port,
    token,
    bridge,
    /// Lets the page fetch `path`, and returns where: a URL on this server.
    grant(path) {
      const id = `${randomBytes(8).toString("hex")}/${encodeURIComponent(basename(path))}`;
      grants.set(id, resolve(path));
      return `/file/${id}?token=${token}`;
    },
    close() {
      bridge.close();
      return new Promise((done) => server.close(done));
    },
  };
}
