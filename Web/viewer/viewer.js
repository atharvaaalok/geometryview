// The universal viewer's page: the engine on a full-window canvas, and the chrome around it.
//
// viewer/?model=<fixture>        opens build/fixtures/<fixture>.geoblob
// viewer/?test=<name>&label=...  runs a scripted check and posts its report (see runTest)

import { Engine, Events } from "./engine.js";
import { ViewportInput } from "./input.js";

const params = new URLSearchParams(location.search);
const canvas = document.getElementById("viewport");
const status = document.getElementById("status");
const phone = /iPhone|iPad|Android/.test(navigator.userAgent) ||
              (navigator.maxTouchPoints > 1 && /Macintosh/.test(navigator.userAgent));

const listeners = new Set();
const waitFor = (kind) => new Promise((resolve) => {
  const listener = (event) => {
    if (event.kind === kind || event.kind === Events.Failed) {
      listeners.delete(listener);
      resolve(event);
    }
  };
  listeners.add(listener);
});

function showStatus(text) {
  status.hidden = !text;
  status.textContent = text ?? "";
}

let engine;
try {
  engine = await Engine.start(canvas, {
    event(kind, document, node, done, total, message) {
      const event = { kind, document, node, done, total, message };
      if (kind === Events.Failed) showStatus(message);
      if (kind === Events.SceneReady) showStatus(null);
      for (const listener of [...listeners]) listener(event);
    },
  }, { phone });
} catch (error) {
  showStatus(`This browser cannot run the viewer: ${error.message}`);
  throw error;
}
new ResizeObserver(() => engine.resize()).observe(canvas);
new ViewportInput(engine, canvas, {
  onCommand: (name) => console.log("command", name),
});
window.geometry = engine; // for the console and the shell's checks

const model = params.get("model");
if (model) engine.open(`fixtures/${model}.geoblob`, `${model}.geoblob`);

if (params.has("test")) runTest(params.get("test")).catch((error) => post({ error: error.message }));

// ---- Scripted checks ----------------------------------------------------------------------

async function post(report) {
  const body = JSON.stringify({ label: `${params.get("label") ?? "unlabelled"}-p4-${params.get("test")}-${model}`,
                                userAgent: navigator.userAgent, devicePixelRatio, ...report });
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) shell.postMessage(body);
  else await fetch("/report", { method: "POST", body }).catch(() => {});
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The view, as a PNG the server keeps beside the fixtures.
async function postSnapshot() {
  const image = await engine.snapshot();
  if (!image) return;
  const picture = new OffscreenCanvas(image.width, image.height);
  picture.getContext("2d").putImageData(
      new ImageData(new Uint8ClampedArray(image.pixels.buffer), image.width, image.height), 0, 0);
  const png = await picture.convertToBlob({ type: "image/png" });
  const label = `${params.get("label") ?? "unlabelled"}-p4-${params.get("test")}-${model}`;
  await fetch(`/png?label=${encodeURIComponent(label)}`, { method: "POST", body: png }).catch(() => {});
}

// Waits for the scene, then measures: frames submitted while nothing happens, frames while the
// pointer drags the view, hover picks, and the memory held.
async function runTest() {
  const opened = performance.now();
  const ready = await waitFor(Events.SceneReady);
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  const loadMs = performance.now() - opened;
  await sleep(1500);
  const settled = engine.stats();
  await sleep(3000);
  const idle = engine.stats();
  // A drag across the middle, at 60 events a second for 2 s.
  const { width, height } = canvas;
  const { Pointer, Buttons } = await import("./engine.js");
  engine.pointer(Pointer.Down, Buttons.Left, width / 2, height / 2);
  const dragStart = engine.stats();
  const started = performance.now();
  for (let i = 1; i <= 120; ++i) {
    engine.pointer(Pointer.Move, Buttons.None, width / 2 + i * 4, height / 2 + i);
    await new Promise(requestAnimationFrame);
  }
  engine.pointer(Pointer.Up, Buttons.Left, width / 2 + 480, height / 2 + 120);
  const dragMs = performance.now() - started;
  const dragged = engine.stats();
  // Hover over the middle, where the model is.
  engine.pointer(Pointer.Move, Buttons.None, width / 2, height / 2);
  await sleep(1000);
  const hover = engine.stats();
  await sleep(2000);
  const after = engine.stats();
  await postSnapshot();
  await post({
    width, height, loadMs: Math.round(loadMs),
    idleFrames: idle.frames - settled.frames, idleSettledFrames: idle.settledFrames - settled.settledFrames,
    looping: idle.looping,
    dragFrames: dragged.frames - dragStart.frames, dragMs: Math.round(dragMs),
    gpuMs: dragged.gpuMs, hovered: hover.hovered,
    settledAfterDrag: after.settledFrames - dragged.settledFrames,
    loopingAfter: after.looping,
    bufferMB: after.bufferMB, targetMB: after.targetMB, heapMB: after.heapMB,
    triangles: after.triangles,
  });
}
