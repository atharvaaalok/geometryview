// The launcher tells an old cache file by its version, which must be the one gcli writes.

import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";
import { kCacheVersion } from "../src/native.mjs";

test("the launcher's cache version is the format's", () => {
  const header = readFileSync(new URL("../../Import/format/blob.h", import.meta.url), "utf8");
  const version = Number(header.match(/inline constexpr u32 kVersion = (\d+);/)[1]);
  assert.equal(kCacheVersion, version);
});
