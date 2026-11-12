// Which machines run the native engine: the Mac extension bundles it, and a Mac that cannot run it
// uses the browser's engine instead.

import { test } from "node:test";
import assert from "node:assert/strict";
import { canRunNative } from "../src/native.mjs";

test("the native engine runs on Apple silicon with macOS 26 or later only", () => {
  assert.equal(canRunNative("darwin", "arm64", "25.0.0"), true);   // macOS 26
  assert.equal(canRunNative("darwin", "arm64", "26.1.0"), true);   // macOS 27
  assert.equal(canRunNative("darwin", "arm64", "24.6.0"), false);  // macOS 15
  assert.equal(canRunNative("darwin", "x64", "25.0.0"), false);    // an Intel Mac
  assert.equal(canRunNative("linux", "arm64", "6.8.0"), false);
  assert.equal(canRunNative("win32", "x64", "10.0.26100"), false);
});

test("the cache keeps within its budget, the files used longest ago going first", async () => {
  const { mkdtemp, writeFile, utimes, readdir, rm } = await import("node:fs/promises");
  const { tmpdir } = await import("node:os");
  const { join } = await import("node:path");
  const { sweepCache } = await import("../src/native.mjs");
  const directory = await mkdtemp(join(tmpdir(), "geometryview-cache-test-"));
  try {
    const now = Date.now();
    // Four files of 100 bytes, used 4, 3, 2 and 1 hours ago, a partial file 2 hours old and one
    // just begun.
    for (const [name, hours] of [["a.geoblob", 4], ["b.geoblob", 3], ["c.geoblob", 2], ["d.geoblob", 1],
                                 ["e.geoblob.1.partial", 2], ["f.geoblob.1.partial", 0]]) {
      await writeFile(join(directory, name), Buffer.alloc(100));
      const when = new Date(now - hours * 3600_000);
      await utimes(join(directory, name), when, when);
    }
    // A budget of 300 bytes: down to 240, so a and b go.
    assert.equal(await sweepCache(directory, 300, now), 200);
    assert.deepEqual((await readdir(directory)).sort(), ["c.geoblob", "d.geoblob", "f.geoblob.1.partial"]);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
