// The toolbar folds its least used tools into More, a rank at a time, until it fits.

import { test } from "node:test";
import assert from "node:assert/strict";
import { foldPlan, ruleShows, widthOf } from "../../Web/viewer/toolbar-fold.js";

// The desktop toolbar, as measured on a Mac: 904 points with nothing folded.
const desktop = [
  { key: "parts", width: 0, group: 0 }, { key: "open", width: 30, group: 0 }, { key: "connect", width: 30, group: 0 },
  { key: "pick", width: 176, group: 1 },
  { key: "hide", width: 30, group: 2 }, { key: "ghost", width: 30, group: 2 }, { key: "isolate", width: 79, group: 2 },
  { key: "showAll", width: 30, group: 2 },
  { key: "undo", width: 30, group: 3 }, { key: "redo", width: 30, group: 3 },
  { key: "seeThrough", width: 30, group: 4 }, { key: "section", width: 30, group: 4 }, { key: "look", width: 30, group: 4 },
  { key: "projection", width: 120, group: 4 }, { key: "views", width: 30, group: 4 }, { key: "fit", width: 30, group: 4 },
  { key: "shortcuts", width: 30, group: 4 },
];

test("a wide window folds nothing", () => {
  assert.equal(foldPlan(desktop, 1104).size, 0);
  assert.ok(widthOf(desktop, new Set()) <= 1104);
});

test("a narrower one folds the least used first, and only as many as it must", () => {
  // Folding one 30-point tool gains nothing, since More takes its place: the next rank goes too.
  const full = widthOf(desktop, new Set());
  assert.deepEqual([...foldPlan(desktop, full - 1)].sort(), ["projection", "shortcuts"]);
  const more = foldPlan(desktop, 642); // a Mac window at its narrowest
  assert.ok(more.has("projection") && more.has("views"));
  assert.ok(!more.has("undo") && !more.has("showAll"));
  assert.ok(widthOf(desktop, more) <= 642);
});

test("the add pair folds together, and a group with nothing left loses its rule", () => {
  const folded = foldPlan(desktop, 560);
  assert.equal(folded.has("open"), folded.has("connect"));
  if (folded.has("open")) assert.equal(ruleShows(desktop, folded, 0), false);
  assert.equal(ruleShows(desktop, folded, 1), true);
});

test("a phone, whose toolbar lacks the desktop's tools, folds until it fits or nothing more can", () => {
  const phone = desktop.map((i) => (["hide", "ghost", "isolate", "shortcuts"].includes(i.key) ? { ...i, width: 0 }
                                      : i.key === "parts" ? { ...i, width: 30 } : i));
  const folded = foldPlan(phone, 386);
  assert.ok(widthOf(phone, folded) <= 386);
  assert.ok(!folded.has("parts") && !folded.has("pick"));
});
