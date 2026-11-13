// What the browser keeps of its imports: keys that tell files of one name apart, and eviction.

import { test } from "node:test";
import assert from "node:assert/strict";
import { evictions, keyOf, pictureKeyOf } from "../../Web/viewer/kept.js";

const file = { source: "Parts", path: "motors/nema17.step", size: 120_331, modified: 1_731_000_000_000, version: 5 };

test("a key is 32 hex digits, the same for the same file", async () => {
  const key = await keyOf(file);
  assert.match(key, /^[0-9a-f]{32}$/);
  assert.equal(await keyOf({ ...file }), key);
});

test("files of one name in two folders, or two places in one, have different keys", async () => {
  const key = await keyOf(file);
  assert.notEqual(await keyOf({ ...file, source: "Archive" }), key);
  assert.notEqual(await keyOf({ ...file, path: "old/nema17.step" }), key);
});

test("a changed file, or another cache version, is a new key", async () => {
  const key = await keyOf(file);
  assert.notEqual(await keyOf({ ...file, size: file.size + 1 }), key);
  assert.notEqual(await keyOf({ ...file, modified: file.modified + 1 }), key);
  assert.notEqual(await keyOf({ ...file, version: 6 }), key);
});

test("fields cannot run into each other", async () => {
  assert.notEqual(await keyOf({ ...file, source: "a", path: "b/c" }), await keyOf({ ...file, source: "a/b", path: "c" }));
});

test("eviction drops the least recently used until the rest fits, sparing what is kept", () => {
  const notes = [
    { key: "old", bytes: 100, usedAt: 1 },
    { key: "open", bytes: 100, usedAt: 2 },
    { key: "mid", bytes: 100, usedAt: 3 },
    { key: "new", bytes: 100, usedAt: 4 },
  ];
  assert.deepEqual(evictions(notes, 400), []);
  assert.deepEqual(evictions(notes, 250, new Set(["open"])), ["old", "mid"]);
  assert.deepEqual(evictions(notes, 0, new Set(["open", "new"])), ["old", "mid"]);
});

test("a picture's key is the one it had for drawing 1, and new for another drawing", async () => {
  const spec = { style: 0, colorMode: 1, pixels: 512, pixelsPerPoint: 2, lineScale: 0.5, folderColor: 1, version: 5 };
  // The key as it was made before the drawing was part of it.
  const before = async (fields) => {
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(JSON.stringify(fields))));
    return [...digest.subarray(0, 8)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  };
  const plain = [0, 1, 512, 2, 0.5, 1, 5];
  assert.equal(await pictureKeyOf(spec), await before(plain));
  assert.equal(await pictureKeyOf({ ...spec, drawing: 1 }), await before(plain));
  assert.equal(await pictureKeyOf({ ...spec, supersample: 2 }), await before([...plain, "ss2"]));
  assert.notEqual(await pictureKeyOf({ ...spec, drawing: 2 }), await pictureKeyOf(spec));
  assert.notEqual(await pictureKeyOf({ ...spec, supersample: 2, drawing: 2 }), await pictureKeyOf({ ...spec, supersample: 2 }));
});
