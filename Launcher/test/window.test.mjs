// How the launcher opens the viewer on Linux, where Chrome needs Vulkan on for WebGPU.
import { test } from "node:test";
import assert from "node:assert/strict";
import { linuxViewerWindow } from "../src/page.mjs";

test("on Linux the viewer opens in a Chrome window of its own, with Vulkan on", () => {
  const saved = { chrome: process.env.GEOMETRY_CHROME, state: process.env.XDG_STATE_HOME };
  delete process.env.GEOMETRY_CHROME;
  delete process.env.XDG_STATE_HOME;
  try {
    const url = "http://127.0.0.1:4100/?token=t";
    const [chrome, args] = linuxViewerWindow(url, "/home/a", (path) => path === "/usr/bin/chromium");
    assert.equal(chrome, "/usr/bin/chromium");
    assert.ok(args.includes("--enable-features=Vulkan"));
    assert.ok(args.includes(`--app=${url}`));
    // Its own profile, so the flags hold even with the person's Chrome already running.
    assert.ok(args.includes("--user-data-dir=/home/a/.local/state/geometry-viewer/chrome"));
    // Without Chrome, the default browser opens it instead.
    assert.equal(linuxViewerWindow(url, "/home/a", () => false), null);
  } finally {
    if (saved.chrome !== undefined) process.env.GEOMETRY_CHROME = saved.chrome;
    if (saved.state !== undefined) process.env.XDG_STATE_HOME = saved.state;
  }
});
