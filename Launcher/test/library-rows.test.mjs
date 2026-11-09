// The Library's rows: what a folder lists and in what order, the sections, the recent files.

import { test } from "node:test";
import assert from "node:assert/strict";
import { ageText, keyOf, listed, remember, RowKinds, rowsOf, Sections } from "../../Web/viewer/library-rows.js";

const base = { folders: [], listings: new Map(), expanded: new Set(), collapsed: new Set(), recent: [] };

test("a folder lists its folders, then its STEP and STL files, in the order numbers read", () => {
  const entries = [
    { name: "part10.step", kind: "file" }, { name: "notes.txt", kind: "file" },
    { name: "part2.STP", kind: "file" }, { name: ".hidden.step", kind: "file" },
    { name: "zeta", kind: "folder" }, { name: "Alpha", kind: "folder" }, { name: "mesh.stl", kind: "file" },
  ];
  assert.deepEqual(listed(entries).map((e) => e.name), ["Alpha", "zeta", "mesh.stl", "part2.STP", "part10.step"]);
});

test("with nothing connected or opened, each section says so", () => {
  const rows = rowsOf(base);
  assert.deepEqual(rows.map((r) => r.kind), [RowKinds.Section, RowKinds.Message, RowKinds.Section, RowKinds.Message]);
  assert.match(rows[1].text, /Connect a folder/);
  assert.match(rows[3].text, /Files you open appear here/);
});

test("an open folder shows its listing a level down, and a file open now is marked", () => {
  const folders = [{ id: "a", name: "Parts", state: "ready" }];
  const listings = new Map([
    [keyOf("a", ""), { state: "listed", entries: [{ name: "gear.step", kind: "file" }, { name: "sub", kind: "folder" }] }],
    [keyOf("a", "sub"), { state: "listed", entries: [] }],
  ]);
  const expanded = new Set([keyOf("a", ""), keyOf("a", "sub")]);
  const rows = rowsOf({ ...base, folders, listings, expanded, open: new Set([keyOf("a", "gear.step")]) });
  const shown = rows.map((r) => [r.kind, r.depth, r.name ?? r.text]);
  assert.deepEqual(shown.slice(0, 5), [
    [RowKinds.Section, 0, "Folders"],
    [RowKinds.Folder, 1, "Parts"],
    [RowKinds.Folder, 2, "sub"],
    [RowKinds.Message, 3, "No STEP or STL files here."],
    [RowKinds.File, 2, "gear.step"],
  ]);
  assert.equal(rows[4].open, true);
  assert.equal(rows[4].path, "gear.step");
});

test("a folder being read says so only once it is slow, and one that failed says so", () => {
  const folders = [{ id: "a", name: "Parts", state: "ready" }];
  const expanded = new Set([keyOf("a", "")]);
  const reading = (slow) => rowsOf({ ...base, folders, expanded,
                                     listings: new Map([[keyOf("a", ""), { state: "reading", slow }]]) });
  assert.equal(reading(false).length, 4);
  assert.equal(reading(true)[2].text, "Reading…");
  const failed = rowsOf({ ...base, folders, expanded, listings: new Map([[keyOf("a", ""), { state: "failed" }]]) });
  assert.equal(failed[2].text, "This folder could not be read.");
});

test("a folder needing permission again doesn't open, and collapsed sections hide their rows", () => {
  const folders = [{ id: "a", name: "Parts", state: "reconnect" }];
  const rows = rowsOf({ ...base, folders, expanded: new Set([keyOf("a", "")]) });
  assert.equal(rows[1].state, "reconnect");
  assert.equal(rows[1].expanded, false);
  const collapsed = rowsOf({ ...base, folders, collapsed: new Set([Sections.Folders, Sections.Recent]) });
  assert.deepEqual(collapsed.map((r) => r.name), ["Folders", "Recent"]);
});

test("a file opened again moves to the top of Recent, and the oldest beyond twelve fall off", () => {
  let recent = [];
  for (let i = 0; i < 13; ++i) recent = remember(recent, { key: `f${i}`, name: `f${i}`, openedAt: i }).recent;
  assert.equal(recent.length, 12);
  assert.equal(recent[0].key, "f12");
  const again = remember(recent, { key: "f5", name: "f5", openedAt: 99 });
  assert.equal(again.recent[0].key, "f5");
  assert.equal(again.recent.filter((r) => r.key === "f5").length, 1);
  assert.deepEqual(remember(recent, { key: "new", name: "new", openedAt: 100 }).dropped.map((r) => r.key), ["f1"]);
});

test("how long ago, in a few characters", () => {
  const now = 1_000_000_000;
  assert.equal(ageText(now - 20_000, now), "now");
  assert.equal(ageText(now - 5 * 60_000, now), "5 min");
  assert.equal(ageText(now - 3 * 3_600_000, now), "3 h");
  assert.equal(ageText(now - 2 * 86_400_000, now), "2 d");
});
