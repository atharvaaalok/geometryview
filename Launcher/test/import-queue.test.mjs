// The browser's import queue, run through the cases it shares with the Mac's library::Queue.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { ImportQueue } from "../../Web/viewer/import-queue.js";

const shared = JSON.parse(readFileSync(new URL("../../Tests/data/gallery-queue.json", import.meta.url), "utf8"));

function run(queue, step) {
  const [[op, value]] = Object.entries(step);
  const where = `${op} ${JSON.stringify(value)}`;
  switch (op) {
    case "want":
      queue.want(value.map(([id, band, order, bytes]) => ({ id, band, order, bytes })));
      break;
    case "next":
      assert.equal(queue.next(), value, where);
      break;
    case "finish":
      queue.finish(value);
      break;
    case "stopped":
      queue.stopped(value);
      break;
    case "cancel":
      assert.deepEqual(queue.toCancel(), value, where);
      break;
    case "waiting":
      assert.deepEqual(queue.waiting(), value, where);
      break;
    case "running":
      assert.deepEqual(queue.running(), value, where);
      break;
    case "slots":
      queue.setSlots(value);
      break;
    case "prefetch":
      queue.setPrefetch(value);
      break;
    default:
      assert.fail(`unknown step ${op}`);
  }
}

for (const c of shared.cases) {
  test(`import queue: ${c.name}`, () => {
    const queue = new ImportQueue({ slots: c.slots ?? 1, prefetch: c.prefetch ?? true });
    for (const step of c.steps) run(queue, step);
  });
}

test("import queue: thousands of wanted files start in order, quickly", () => {
  const queue = new ImportQueue({ slots: 4 });
  const wants = Array.from({ length: 20000 }, (_, i) => ({ id: String(i), band: i < 40 ? "visible" : "prefetch", order: i }));
  const start = performance.now();
  for (let scroll = 0; scroll < 50; scroll++) queue.want(wants);
  let started = 0;
  for (let id = queue.next(); id !== null; id = queue.next()) {
    assert.equal(Number(id), started);
    queue.finish(id);
    started++;
  }
  assert.equal(started, 20000);
  assert.ok(queue.idle);
  assert.ok(performance.now() - start < 2000, "slow");
});
