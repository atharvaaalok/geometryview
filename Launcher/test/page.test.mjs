// The whole chain the AI tools use where there is no native engine: the launcher's server, the
// viewer's page in a browser, and the bridge between them, opening a model and drawing a picture.
// It needs Chrome and WebGPU, so it runs only with GEOMETRYVIEW_E2E=1. GEOMETRYVIEW_CHROME names
// another browser and GEOMETRYVIEW_CHROME_FLAGS adds flags, as on Linux without a GPU;
// GEOMETRYVIEW_E2E_PICTURE saves the picture drawn.

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { fileURLToPath } from "node:url";
import { after, before, test } from "node:test";
import assert from "node:assert/strict";
import { startViewer } from "../src/web.mjs";

const chromePath = process.env.GEOMETRYVIEW_CHROME ?? {
  darwin: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  win32: "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  linux: "/usr/bin/google-chrome",
}[process.platform];
const chromeFlags = (process.env.GEOMETRYVIEW_CHROME_FLAGS ?? "").split(" ").filter(Boolean);
// The gearbox, from the repository's test-models folder (see mcp.test.mjs) or GEOMETRYVIEW_FIXTURES.
const fixtures = process.env.GEOMETRYVIEW_FIXTURES ??
                 fileURLToPath(new URL("../../test-models/", import.meta.url));
const model = join(fixtures, "models/ple120_gearbox.step");
const enabled = process.env.GEOMETRYVIEW_E2E === "1" && existsSync(chromePath) && existsSync(model);

let viewer, chrome, profile;

before(async () => {
  if (!enabled) return;
  viewer = await startViewer();
  profile = mkdtempSync(join(tmpdir(), "geometry-e2e-"));
  chrome = spawn(chromePath, ["--headless=new", "--enable-unsafe-webgpu", `--user-data-dir=${profile}`,
                              "--window-size=1400,900", ...chromeFlags, viewer.url], { stdio: "ignore" });
  await viewer.bridge.page(30_000);
});

after(async () => {
  if (chrome) {
    const exited = new Promise((resolve) => chrome.once("exit", resolve));
    chrome.kill();
    await exited;
  }
  await viewer?.close();
  if (profile) rmSync(profile, { recursive: true, force: true });
});

test("the page opens a STEP file, lists it and pictures it", { skip: !enabled }, async () => {
  const info = await stat(model);
  const described = await viewer.bridge.request("open", {
    url: viewer.grant(model), name: basename(model), lastModified: info.mtimeMs,
  });
  assert.equal(described.parts, 37);
  assert.equal(described.bodies, 89);
  // Every segment arrived, the last ones too, written as the import finished.
  assert.equal(described.triangles, 116_024, JSON.stringify(described));

  const nodes = await viewer.bridge.request("structure", {});
  assert.ok(nodes.some((node) => node.name === "housing" && node.kind === "assembly"));

  const picture = await viewer.bridge.request("render", {
    azimuth: 0, elevation: 0, style: 1, width: 400, height: 300,
    section: [{ axis: "x", at: 0.5 }],
  });
  const png = Buffer.from(picture.png, "base64");
  const keep = process.env.GEOMETRYVIEW_E2E_PICTURE;
  if (keep) writeFileSync(keep, png);
  assert.equal(png.subarray(1, 4).toString(), "PNG");
  assert.equal(png.readUInt32BE(16), 400);
  assert.equal(png.readUInt32BE(20), 300);

  await assert.rejects(viewer.bridge.request("render", { isolate: ["no such part"] }),
                       /No part's name contains/);
});
