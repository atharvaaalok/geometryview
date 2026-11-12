// The chrome's icons are drawn by the page and by the Mac app alike, from Web/viewer/icons.js. The
// app reads that file line by line, as this does, so every icon must stay in that form.

import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";

const source = new URL("../../Web/viewer/icons.js", import.meta.url);

// What App/Sources/Design/Icons.swift reads: `  name: \`markup\`,` on one line.
export function readIcons(text) {
  const icons = {};
  for (const [, name, markup] of text.matchAll(/^ {2}(\w+): `([^`$]*)`,$/gm)) icons[name] = markup;
  return icons;
}

test("every icon is on a line of its own, as the Mac app reads them", async () => {
  const { iconMarkup } = await import(source.href);
  assert.deepEqual(readIcons(readFileSync(source, "utf8")), iconMarkup);
});

test("the icons the chrome uses are all there", async () => {
  const { iconMarkup } = await import(source.href);
  for (const name of ["eye", "eyeSlash", "ghost", "scope", "undo", "redo", "seeThrough", "scissors",
                      "palette", "cube", "fit", "chevron", "assembly", "search", "folder",
                      "folderPlus", "file", "plus", "close", "parts", "plates", "sheet", "ladders",
                      "atlas", "catalogue", "poster"]) {
    assert.ok(iconMarkup[name], name);
  }
});
