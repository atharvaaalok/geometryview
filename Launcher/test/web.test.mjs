// The viewer's server: what it serves, to whom, and the bridge to its page.

import { request as httpRequest } from "node:http";
import { writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { startViewer, viewerMounts } from "../src/web.mjs";

let viewer;
before(async () => { viewer = await startViewer(); });
after(() => viewer.close());

// A GET with the Host header given, which fetch would not let a test choose.
function get(path, host = `127.0.0.1:${viewer.port}`) {
  return new Promise((resolve, reject) => {
    const req = httpRequest({ host: "127.0.0.1", port: viewer.port, path, headers: { host } }, (response) => {
      const chunks = [];
      response.on("data", (c) => chunks.push(c));
      response.on("end", () => resolve({ status: response.statusCode, headers: response.headers,
                                         body: Buffer.concat(chunks) }));
    });
    req.on("error", reject);
    req.end();
  });
}

test("the page is served cross-origin isolated", async () => {
  const page = await get("/");
  assert.equal(page.status, 200);
  assert.match(page.headers["content-type"], /text\/html/);
  assert.equal(page.headers["cross-origin-opener-policy"], "same-origin");
  assert.equal(page.headers["cross-origin-embedder-policy"], "require-corp");
  assert.match(page.body.toString(), /viewer\.js/);
});

// The engine is a build output: a checkout without the wasm-webgpu preset built has none to serve.
const engineBuilt = viewerMounts().some(([prefix, folder]) =>
  existsSync(join(folder, prefix === "/build/" ? "geometryview-web.wasm" : "build/geometryview-web.wasm")));

test("the engine is served as WebAssembly", { skip: !engineBuilt && "the engine is not built" }, async () => {
  const engine = await get("/build/geometryview-web.wasm");
  assert.equal(engine.status, 200);
  assert.equal(engine.headers["content-type"], "application/wasm");
});

test("a request naming another host is refused", async () => {
  assert.equal((await get("/", "evil.example:80")).status, 421);
  assert.equal((await get("/", `localhost:${viewer.port}`)).status, 200);
});

test("paths cannot leave the viewer's folders", async () => {
  assert.notEqual((await get("/..%2f..%2fpackage.json")).status, 200);
  assert.notEqual((await get("/../../../etc/passwd")).status, 200);
});

test("a model file is served only when granted, and only with the token", async () => {
  const path = join(tmpdir(), `geometry-launcher-${process.pid}.step`);
  await writeFile(path, "ISO-10303-21;");
  try {
    const url = viewer.grant(path);
    assert.equal((await get(url)).body.toString(), "ISO-10303-21;");
    assert.equal((await get(url.replace(viewer.token, "0".repeat(32)))).status, 403);
    assert.equal((await get(`/file/nothing?token=${viewer.token}`)).status, 403);
  } finally {
    await rm(path, { force: true });
  }
});

test("the bridge takes the page's answers, and refuses strangers", async () => {
  const bridgeUrl = `ws://127.0.0.1:${viewer.port}/bridge?token=${viewer.token}`;
  // A page that answers each request with its method and parameters.
  const page = new WebSocket(bridgeUrl, { headers: { origin: viewer.address } });
  await new Promise((resolve, reject) => {
    page.onopen = resolve;
    page.onerror = () => reject(new Error("the bridge refused the page"));
  });
  page.onmessage = (event) => {
    const { id, method, params } = JSON.parse(event.data);
    if (method === "fail") page.send(JSON.stringify({ id, error: "it failed" }));
    else page.send(JSON.stringify({ id, result: { method, params, big: "x".repeat(200_000) } }));
  };
  const answer = await viewer.bridge.request("render", { style: "metal" });
  assert.equal(answer.method, "render");
  assert.deepEqual(answer.params, { style: "metal" });
  assert.equal(answer.big.length, 200_000);
  await assert.rejects(viewer.bridge.request("fail"), /it failed/);
  page.close();

  for (const [url, origin] of [[bridgeUrl.replace(viewer.token, "0".repeat(32)), viewer.address],
                               [bridgeUrl, "http://evil.example"]]) {
    const stranger = new WebSocket(url, { headers: { origin } });
    const opened = await new Promise((resolve) => {
      stranger.onopen = () => resolve(true);
      stranger.onerror = () => resolve(false);
    });
    assert.equal(opened, false);
  }
});

test("the page has its manifest and icons, so a browser can install it", async () => {
  const manifest = await get("/manifest.webmanifest");
  assert.equal(manifest.status, 200);
  assert.equal(manifest.headers["content-type"], "application/manifest+json");
  assert.equal(JSON.parse(manifest.body.toString()).name, "GeometryView");
  const icon = await get("/icons/icon-192.png");
  assert.equal(icon.status, 200);
  assert.equal(icon.headers["content-type"], "image/png");
});
