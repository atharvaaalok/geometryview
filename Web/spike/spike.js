// Runs the renderer spike: loads a cache fixture, renders a scripted orbit at three zooms, and
// reports GPU and frame timings. Reports go to the spike server, or to the iOS shell when inside it.

import createSpike from "./build/spike_webgpu.mjs";

const params = new URLSearchParams(location.search);
const model = params.get("model") ?? "stl10m";
const width = Number(params.get("w") ?? 2880);
const height = Number(params.get("h") ?? 1800);
const copies = Number(params.get("copies") ?? 1);
const mode = params.get("mode") === "indexed" ? "indexed" : "pulled";
// Edges, shadow and occlusion, as bits; all by default. Line widths follow the backing scale.
const features = Number(params.get("features") ?? 7);
const pixelsPerPoint = Number(params.get("ppp") ?? devicePixelRatio);
const label = `${params.get("label") ?? "unlabelled"}-${model}${copies > 1 ? `x${copies}` : ""}` +
              `-${width}x${height}-${mode}${mode === "indexed" && features !== 7 ? `-f${features}` : ""}`;
const status = document.getElementById("status");
const canvas = document.getElementById("canvas");
canvas.width = width;
canvas.height = height;

async function post(report) {
  const body = JSON.stringify({ label, ...report });
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) {
    shell.postMessage(body);
    return;
  }
  await fetch("/report", { method: "POST", body }).catch(() => {});
}

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(spike, wanted) {
  for (;;) {
    const state = spike._spike_state();
    if (state < 0) {
      throw new Error(spike.UTF8ToString(spike._spike_error()));
    }
    if (state >= wanted) {
      return;
    }
    await wait(20);
  }
}

try {
  const spike = await createSpike();
  status.textContent = "Asking for a WebGPU device…";
  spike._spike_init(width, height);
  await until(spike, 1);

  status.textContent = `Loading ${model}…`;
  let started = performance.now();
  const bytes = new Uint8Array(await (await fetch(`fixtures/${model}.geoblob`)).arrayBuffer());
  const fetchMs = performance.now() - started;
  started = performance.now();
  const pointer = spike._malloc(bytes.length);
  spike.HEAPU8.set(bytes, pointer);
  const loaded = spike._spike_load(pointer, bytes.length, copies, mode === "indexed" ? 1 : 0,
                                   features, pixelsPerPoint);
  spike._free(pointer);
  if (loaded !== 0) {
    await until(spike, 99);
  }
  const uploadMs = performance.now() - started;

  status.textContent = `Orbiting ${model} at ${width}×${height}…`;
  spike._spike_start();
  await until(spike, 3);
  const results = JSON.parse(spike.UTF8ToString(spike._spike_results()));
  const report = { userAgent: navigator.userAgent, devicePixelRatio, model, copies, mode, features,
                   pixelsPerPoint,
                   fileMB: +(bytes.length / 2 ** 20).toFixed(1), fetchMs: Math.round(fetchMs),
                   uploadMs: Math.round(uploadMs), ...results };
  status.textContent = JSON.stringify(report, null, 1);
  await post(report);
} catch (error) {
  status.textContent = `Failed: ${error.message ?? error}`;
  await post({ error: String(error.message ?? error) });
}
