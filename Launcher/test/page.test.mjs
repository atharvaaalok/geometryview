// The whole chain the AI tools use where there is no native engine: the launcher's server, the
// viewer's page in a browser, and the bridge between them, opening a model and drawing a picture.
// It needs Chrome and a GPU, so it runs only with GEOMETRY_E2E=1.

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { after, before, test } from "node:test";
import assert from "node:assert/strict";
import { startViewer } from "../src/web.mjs";

const chromePath = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const model = new URL("../../build/fixtures/step/ple120.step", import.meta.url).pathname;
const enabled = process.env.GEOMETRY_E2E === "1" && existsSync(chromePath) && existsSync(model);

let viewer, chrome, profile;

before(async () => {
  if (!enabled) return;
  viewer = await startViewer();
  profile = mkdtempSync(join(tmpdir(), "geometry-e2e-"));
  chrome = spawn(chromePath, ["--headless=new", "--enable-unsafe-webgpu", `--user-data-dir=${profile}`,
                              "--window-size=1400,900", viewer.url], { stdio: "ignore" });
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
  assert.ok(described.triangles > 100_000);

  const nodes = await viewer.bridge.request("structure", {});
  assert.ok(nodes.some((node) => node.name === "housing" && node.kind === "assembly"));

  const picture = await viewer.bridge.request("render", {
    azimuth: 0, elevation: 0, style: 1, width: 400, height: 300,
    section: [{ axis: "x", at: 0.5 }],
  });
  const png = Buffer.from(picture.png, "base64");
  assert.equal(png.subarray(1, 4).toString(), "PNG");
  assert.equal(png.readUInt32BE(16), 400);
  assert.equal(png.readUInt32BE(20), 300);

  await assert.rejects(viewer.bridge.request("render", { isolate: ["no such part"] }),
                       /No part's name contains/);
});
