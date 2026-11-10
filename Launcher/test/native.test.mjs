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
