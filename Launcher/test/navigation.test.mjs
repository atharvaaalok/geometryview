// Back and forward in the web viewer, run through the cases it shares with the Mac's
// Navigation.swift (--navigation-check).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { Navigation, kNavigationLimit, placeKey } from "../../Web/viewer/navigation.js";

const shared = JSON.parse(readFileSync(new URL("../../Tests/data/navigation.json", import.meta.url), "utf8"));

// "folder:<root>|<path>" or "files:<path>,<path>", as a place.
function place(text) {
  if (text === null) return null;
  const [kind, rest] = [text.slice(0, text.indexOf(":")), text.slice(text.indexOf(":") + 1)];
  if (kind === "folder") {
    const [root, path] = rest.split("|");
    return { kind, root, path };
  }
  return { kind: "files", files: rest.split(",") };
}

test("the limit is the one the Mac keeps", () => {
  assert.equal(kNavigationLimit, shared.limit);
});

for (const c of shared.cases) {
  test(c.name, () => {
    const navigation = new Navigation();
    for (const [i, e] of c.events.entries()) {
      const where = `event ${i} (${e.do} ${e.place ?? ""})`;
      if (e.do === "visitMany") {
        for (let n = 0; n < e.count; n += 1) navigation.visit(place(`${e.prefix}${n}`));
        assert.equal(navigation.steps.length, e.length, where);
        assert.equal(placeKey(navigation.steps[0].place), e.first, where);
        assert.equal(navigation.at, e.at, where);
        continue;
      }
      if (e.do === "visit" || e.do === "add") {
        assert.equal(navigation[e.do](place(e.place)), e.move, where);
      } else {
        const step = navigation[e.do]();
        assert.equal(step === null ? null : placeKey(step.place), e.place, where);
      }
      assert.deepEqual(navigation.steps.map((s) => placeKey(s.place)), e.steps, where);
      assert.equal(navigation.at, e.at, where);
    }
  });
}

test("what a folder showed is kept for its own step only", () => {
  const navigation = new Navigation();
  navigation.visit(place("folder:/lib|gears"));
  navigation.keep(place("folder:/lib|"), { scroll: 10 });
  assert.equal(navigation.current.shown, null);
  navigation.keep(place("folder:/lib|gears"), { scroll: 40 });
  assert.deepEqual(navigation.current.shown, { scroll: 40 });
});
