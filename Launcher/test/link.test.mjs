// The viewer offered as a link, for an AI app to open in its own browser: nothing is launched, and
// the model, with the parts and look asked for, goes to the first page that connects.

import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, test } from "node:test";
import assert from "node:assert/strict";

process.env.GEOMETRYVIEW_PORT = "0"; // any free port, beside a viewer the person may be running
const { linkToPage, viewerServer } = await import("../src/page.mjs");

const folder = await mkdtemp(join(tmpdir(), "geometryview-link-"));
const model = join(folder, "rotor.stl");
await writeFile(model, "solid s\nendsolid s\n");

// A page as the bridge sees one: it answers every request at once, and keeps what it was asked.
function fakePage() {
  const asked = [];
  const connection = {
    send(text) {
      const { id, method, params } = JSON.parse(text);
      asked.push({ method, params });
      setImmediate(() => connection.onmessage(JSON.stringify({ id, result: { names: ["rotor.stl"] } })));
    },
    close() {},
  };
  return { connection, asked };
}

after(async () => {
  (await viewerServer()).close();
  await rm(folder, { recursive: true });
  delete process.env.GEOMETRYVIEW_PORT;
});

test("a link opens nothing, and the first page to connect shows the model as asked", async () => {
  const { url, shown } = await linkToPage(model, { isolate: ["rotor"], style: 1 });
  assert.equal(shown, false);
  assert.match(url, /^http:\/\/127\.0\.0\.1:\d+\/\?token=[0-9a-f]+$/);

  const page = fakePage();
  (await viewerServer()).bridge.attach(page.connection);
  for (let i = 0; i < 100 && page.asked.length < 2; ++i) await new Promise((done) => setTimeout(done, 10));
  assert.deepEqual(page.asked.map((a) => a.method), ["open", "show"]);
  assert.equal(page.asked[0].params.name, "rotor.stl");
  assert.deepEqual(page.asked[1].params, { isolate: ["rotor"], style: 1 });
});

test("with a page connected, a link shows the model there at once", async () => {
  const page = fakePage();
  (await viewerServer()).bridge.attach(page.connection);
  const { shown } = await linkToPage(model, { isolate: ["magnet"] });
  assert.equal(shown, true);
  assert.deepEqual(page.asked.map((a) => a.method), ["describe", "show"]);
  assert.deepEqual(page.asked[1].params, { isolate: ["magnet"] });
});

test("a link to a file that isn't a model fails at once", async () => {
  await assert.rejects(linkToPage(join(folder, "missing.step")), /There is no file/);
  const text = join(folder, "notes.txt");
  await writeFile(text, "");
  await assert.rejects(linkToPage(text), /Only STEP/);
});
