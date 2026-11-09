// The viewer's port: the same one each run, so the browser keeps its library, and a second
// launcher that finds it taken tells whose it is.

import { createServer } from "node:net";
import { test } from "node:test";
import assert from "node:assert/strict";
import { kDefaultPort, startViewer, viewerAt } from "../src/web.mjs";

test("the usual port is fixed", () => {
  assert.equal(kDefaultPort, 47700);
});

test("a viewer says it is one, and nothing else does", async () => {
  const viewer = await startViewer();
  assert.equal(await viewerAt(viewer.port), true);
  await viewer.close();
  assert.equal(await viewerAt(viewer.port), false);

  const other = createServer((socket) => socket.end("HTTP/1.1 404 Not Found\r\n\r\n"));
  await new Promise((done) => other.listen(0, "127.0.0.1", done));
  assert.equal(await viewerAt(other.address().port), false);
  other.close();
});

test("a taken port sends the viewer to any free one", async () => {
  const other = createServer();
  await new Promise((done) => other.listen(0, "127.0.0.1", done));
  const taken = other.address().port;
  process.env.GEOMETRYVIEW_PORT = String(taken);
  const { viewerServer } = await import("../src/page.mjs");
  const viewer = await viewerServer();
  assert.notEqual(viewer.port, taken);
  assert.equal(await viewerAt(viewer.port), true);
  await viewer.close();
  other.close();
  delete process.env.GEOMETRYVIEW_PORT;
});
