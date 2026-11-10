// Drives the tool server as an AI app does: start it, initialise, list tools and call them.

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { after, before, test } from "node:test";
import assert from "node:assert/strict";

// The models are in the repository's test-models folder, which git ignores (see test-models/README.md
// there), or under GEOMETRYVIEW_FIXTURES. A test whose model is missing is skipped.
const fixtures = process.env.GEOMETRYVIEW_FIXTURES ??
                 fileURLToPath(new URL("../../test-models/", import.meta.url));
const model = join(fixtures, "models/u8ii_kv100_motor.step");

let server;
let nextId = 1;
const pending = new Map();

function call(method, params) {
  const id = nextId++;
  server.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  return new Promise((resolve) => pending.set(id, resolve));
}

before(() => {
  server = spawn(process.execPath, [fileURLToPath(new URL("../bin/geometryview.mjs", import.meta.url)),
                                    "--mcp"], { stdio: ["pipe", "pipe", "inherit"] });
  let buffered = "";
  server.stdout.on("data", (chunk) => {
    buffered += chunk;
    let newline;
    while ((newline = buffered.indexOf("\n")) >= 0) {
      const message = JSON.parse(buffered.slice(0, newline));
      buffered = buffered.slice(newline + 1);
      pending.get(message.id)?.(message);
      pending.delete(message.id);
    }
  });
});

after(() => server.kill());

test("it introduces itself in the client's protocol version", async () => {
  const { result } = await call("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "test", version: "1" },
  });
  assert.equal(result.protocolVersion, "2025-06-18");
  assert.ok(result.capabilities.tools);
  assert.match(result.instructions, /open_model/);
  server.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
});

test("it lists its four tools with schemas", async () => {
  const { result } = await call("tools/list", {});
  assert.deepEqual(result.tools.map((tool) => tool.name).sort(),
                   ["get_structure", "open_in_viewer", "open_model", "render_view"]);
  for (const tool of result.tools) {
    assert.equal(tool.inputSchema.type, "object");
  }
});

test("render_view offers every drawing style, studio last", async () => {
  const { result } = await call("tools/list", {});
  const render = result.tools.find((tool) => tool.name === "render_view");
  assert.deepEqual(render.inputSchema.properties.style.enum,
                   ["illustration", "metal", "clay", "blueprint", "technical", "studio"]);
});

test("a missing file is a tool error the model can read", async () => {
  const { result } = await call("tools/call", {
    name: "open_model",
    arguments: { path: "/nowhere/part.step" },
  });
  assert.equal(result.isError, true);
  assert.match(result.content[0].text, /no file/);
});

test("it opens a model, lists its parts and pictures one alone", { skip: !existsSync(model) },
     async () => {
  const opened = await call("tools/call", { name: "open_model", arguments: { path: model } });
  assert.notEqual(opened.result.isError, true, opened.result.content[0].text);
  assert.match(opened.result.content[0].text, /19 parts/);
  assert.match(opened.result.content[0].text, /stator — assembly, 5 parts/);

  const structure = await call("tools/call", {
    name: "get_structure",
    arguments: { path: model, filter: "magnet" },
  });
  assert.match(structure.result.content[0].text, /magnets — part, 42 bodies/);
  assert.doesNotMatch(structure.result.content[0].text, /housing/);

  const picture = await call("tools/call", {
    name: "render_view",
    arguments: { path: model, isolate: ["rotor"], view: "front", width: 400, height: 300,
                 section: [{ axis: "x", at: 0.5 }] },
  });
  const image = picture.result.content.find((item) => item.type === "image");
  assert.equal(image.mimeType, "image/png");
  assert.equal(Buffer.from(image.data, "base64").subarray(1, 4).toString(), "PNG");

  const studio = await call("tools/call", {
    name: "render_view",
    arguments: { path: model, style: "studio", width: 400, height: 300 },
  });
  assert.notEqual(studio.result.isError, true, studio.result.content[0].text);
  assert.match(studio.result.content.find((item) => item.type === "text").text, /studio/);

  const nothing = await call("tools/call", {
    name: "render_view",
    arguments: { path: model, isolate: ["no such part"] },
  });
  assert.equal(nothing.result.isError, true);
  assert.match(nothing.result.content[0].text, /No part's name contains/);
});
