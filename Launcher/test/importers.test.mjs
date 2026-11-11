// The page's importers: what a person opens never waits behind the gallery's imports.

import { test } from "node:test";
import assert from "node:assert/strict";
import { Importers } from "../../Web/viewer/importers.js";

// A job that runs until told to end, or until aborted; it records when it ran.
function controlled(log, name) {
  let end;
  const done = new Promise((resolve) => { end = resolve; });
  const run = (signal) => {
    log.push(`start ${name}`);
    return new Promise((resolve, reject) => {
      signal.addEventListener("abort", () => {
        log.push(`abort ${name}`);
        reject(new Error("aborted"));
      });
      done.then(() => {
        log.push(`end ${name}`);
        resolve({ type: "done", name });
      });
    });
  };
  return { run, end: () => end() };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

test("files opened run one at a time, in the order asked", async () => {
  const log = [];
  const importers = new Importers();
  const a = controlled(log, "a"), b = controlled(log, "b");
  const first = importers.live("a", a.run);
  const second = importers.live("b", b.run);
  await tick();
  assert.deepEqual(log, ["start a"]);
  a.end();
  assert.equal((await first).name, "a");
  await tick();
  assert.deepEqual(log, ["start a", "end a", "start b"]);
  b.end();
  assert.equal((await second).name, "b");
});

test("a file being opened cancels a refinement running, which is not tried again", async () => {
  const log = [];
  const importers = new Importers();
  const refine = controlled(log, "refine"), open = controlled(log, "open");
  const refined = importers.live("refine", refine.run, { band: "visible" });
  await tick();
  const opened = importers.live("open", open.run);
  await tick();
  assert.deepEqual(await refined, { type: "cancelled" });
  await tick();
  assert.deepEqual(log, ["start refine", "abort refine", "start open"]);
  open.end();
  await opened;
  await tick();
  assert.deepEqual(importers.running(), { live: [], gallery: [] });
});

test("the gallery starts nothing while a file is being opened, and its importer runs alongside", async () => {
  const log = [];
  const importers = new Importers();
  const g1 = controlled(log, "g1"), g2 = controlled(log, "g2"), open = controlled(log, "open");
  const done = [];
  importers.want([{ id: "g1", band: "visible", order: 0, run: g1.run }, { id: "g2", band: "visible", order: 1, run: g2.run }],
                 { onDone: (id) => done.push(id) });
  await tick();
  assert.deepEqual(log, ["start g1"]);
  const opened = importers.live("open", open.run);
  await tick();
  // Its own importer: the gallery's running import runs on.
  assert.deepEqual(log, ["start g1", "start open"]);
  g1.end();
  await tick();
  await tick();
  assert.deepEqual(done, ["g1"]);
  assert.ok(!log.includes("start g2"), "g2 waits while the file opens");
  open.end();
  await opened;
  await tick();
  await tick();
  assert.ok(log.includes("start g2"));
});

test("on a phone one importer is shared: the gallery's import makes way for an open, then runs again", async () => {
  const log = [];
  const importers = new Importers({ shared: true });
  const g = controlled(log, "g"), open = controlled(log, "open");
  importers.want([{ id: "g", band: "visible", order: 0, run: g.run }]);
  await tick();
  const opened = importers.live("open", open.run);
  await tick();
  await tick();
  assert.deepEqual(log.slice(0, 3), ["start g", "abort g", "start open"]);
  open.end();
  await opened;
  await tick();
  await tick();
  assert.equal(log.filter((line) => line === "start g").length, 2);
});

test("on a phone nothing is prefetched", async () => {
  const log = [];
  const importers = new Importers({ shared: true });
  const p = controlled(log, "p");
  importers.want([{ id: "p", band: "prefetch", order: 0, run: p.run }]);
  importers.setPrefetch(true);
  await tick();
  assert.deepEqual(log, []);
});

test("a gallery job no longer wanted is dropped before it starts", async () => {
  const log = [];
  const importers = new Importers();
  const a = controlled(log, "a"), b = controlled(log, "b");
  importers.want([{ id: "a", band: "visible", order: 0, run: a.run }, { id: "b", band: "near", order: 0, run: b.run }]);
  await tick();
  importers.want([{ id: "a", band: "visible", order: 0, run: a.run }]);
  a.end();
  await tick();
  await tick();
  assert.deepEqual(log, ["start a", "end a"]);
});

test("a waiting open can be cancelled before it starts", async () => {
  const log = [];
  const importers = new Importers();
  const a = controlled(log, "a"), b = controlled(log, "b");
  const first = importers.live("a", a.run);
  const second = importers.live("b", b.run);
  importers.cancel("b");
  assert.deepEqual(await second, { type: "cancelled" });
  a.end();
  await first;
  await tick();
  assert.deepEqual(log, ["start a", "end a"]);
});
