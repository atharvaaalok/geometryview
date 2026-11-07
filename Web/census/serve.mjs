// Serves the census page on this machine only, with the headers the universal viewer will need
// (cross-origin isolation for WebAssembly threads), and saves each browser's report.
//
// Usage: node Web/census/serve.mjs [port]
// Open http://127.0.0.1:<port>/?label=<where it runs> in each browser or app pane.

import { createServer } from "node:http";
import { readFile, writeFile, mkdir, rm } from "node:fs/promises";
import { extname, join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const reports = join(here, "../../Docs/measurements/phase0/census");
const port = Number(process.argv[2] ?? 7341);
const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
                ".wasm": "application/wasm", ".json": "application/json" };

const server = createServer(async (request, response) => {
  const headers = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Cache-Control": "no-store",
  };
  const url = new URL(request.url, `http://${request.headers.host}`);
  if (request.method === "POST" && url.pathname === "/report") {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const report = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    const label = String(report.label ?? "unlabelled").replace(/[^\w.-]+/g, "-");
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
  const name = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
  if (name.includes("..")) {
    response.writeHead(400, headers).end();
    return;
  }
  try {
    const body = await readFile(join(here, name));
    response.writeHead(200, { ...headers, "Content-Type": types[extname(name)] ?? "text/plain" });
    response.end(body);
  } catch {
    response.writeHead(404, headers).end();
  }
});

server.listen(port, "127.0.0.1", () => {
  console.log(`census at http://127.0.0.1:${port}/?label=<where>`);
});
