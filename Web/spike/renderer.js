// Times the viewer's WebGPU renderer, as the spike page timed the Phase 0 spike: loads a cache
// fixture, orbits it at three zooms with a hover sweep, and reports GPU, frame and submit times.
// Reports go to the spike server, or to the iOS shell when inside it.
//
// renderer.html?model=stl10m&w=2880&h=1800&style=0&samples=1&burst=1&phase=6&label=<where>
//   capture   also reads one frame back and posts its pixels to the server
//   reduced   leaves out edges and the shadow, as heavy scenes in motion are drawn
//   thumbs=N  draws the model N times as the gallery's pictures, `size` pixels square (256), on a
//             clear sheet, each handed back without waiting, and reports their timings instead

import createBench from "./bench/geometryview-webgpu-bench.mjs";

const params = new URLSearchParams(location.search);
const model = params.get("model") ?? "stl10m";
const width = Number(params.get("w") ?? 2880);
const height = Number(params.get("h") ?? 1800);
const style = Number(params.get("style") ?? 0); // Illustration
const samples = Number(params.get("samples") ?? 1);
// Frames per display refresh. Above 1 the GPU never idles, so its clocks stay at their highest and
// GPU times measure capacity rather than how far the clocks were lowered to fit the display.
const burst = Number(params.get("burst") ?? 1);
const phase = Number(params.get("phase") ?? 6);
// Edges and the shadow left out, as the engine draws heavy scenes in motion.
const reduced = params.has("reduced");
const label = `${params.get("label") ?? "unlabelled"}-p3-${model}-${width}x${height}-s${style}` +
              `${samples > 1 ? `-x${samples}` : ""}${reduced ? "-reduced" : ""}` +
              `${burst > 1 ? `-burst${burst}` : ""}` +
              `${phase !== 6 ? `-soak${3 * phase}s` : ""}`;
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

async function until(bench, wanted) {
  for (;;) {
    const state = bench._bench_state();
    if (state < 0) {
      throw new Error(bench.UTF8ToString(bench._bench_error()));
    }
    if (state >= wanted) {
      return;
    }
    await wait(20);
  }
}

try {
  const bench = await createBench();
  status.textContent = "Asking for a WebGPU device…";
  bench._bench_init(width, height);
  await until(bench, 1);

  if (params.has("thumbs")) {
    const count = Number(params.get("thumbs") || 60);
    const size = Number(params.get("size") ?? 256);
    status.textContent = `Drawing ${model} ${count} times at ${size} px…`;
    const bytes = new Uint8Array(await (await fetch(`fixtures/${model}.geoblob`)).arrayBuffer());
    const pointer = bench._malloc(bytes.length);
    bench.HEAPU8.set(bytes, pointer);
    const started = performance.now();
    bench._bench_thumbnails(pointer, bytes.length, count, size, style);
    bench._free(pointer);
    await until(bench, 5);
    const wallMs = performance.now() - started;
    const results = JSON.parse(bench.UTF8ToString(bench._bench_results()));
    const report = { userAgent: navigator.userAgent, model, style, size, count,
                     fileKB: Math.round(bytes.length / 1024), wallMs: Math.round(wallMs),
                     perPictureMs: +(wallMs / count).toFixed(2),
                     heapMB: Math.round(bench.HEAPU8.length / 2 ** 20), ...results };
    status.textContent = JSON.stringify(report, null, 1);
    await post({ ...report, label: `${params.get("label") ?? "unlabelled"}-thumbs-${model}-${size}-s${style}` });
  } else {
    status.textContent = `Loading ${model}…`;
    let started = performance.now();
    const bytes = new Uint8Array(await (await fetch(`fixtures/${model}.geoblob`)).arrayBuffer());
    const fetchMs = performance.now() - started;
    started = performance.now();
    const pointer = bench._malloc(bytes.length);
    bench.HEAPU8.set(bytes, pointer);
    bench._bench_load(pointer, bytes.length, style, samples);
    bench._free(pointer);
    await until(bench, 2);
    const loadMs = performance.now() - started;

    status.textContent = `Orbiting ${model} at ${width}×${height}…`;
    bench._bench_reduced(reduced ? 1 : 0);
    bench._bench_start(burst, phase);
    await until(bench, 3);
    const results = JSON.parse(bench.UTF8ToString(bench._bench_results()));
    const report = { userAgent: navigator.userAgent, devicePixelRatio, model, style, samples, burst,
                     phaseSeconds: phase, fileMB: +(bytes.length / 2 ** 20).toFixed(1),
                     fetchMs: Math.round(fetchMs), loadMs: Math.round(loadMs),
                     heapMB: Math.round(bench.HEAPU8.length / 2 ** 20), ...results };
    status.textContent = JSON.stringify(report, null, 1);
    if (params.has("capture")) {
      // One frame read back, as the server keeps it for comparing with Metal's.
      bench._bench_capture();
      await until(bench, 4);
      const pixels = bench.HEAPU8.slice(bench._bench_capture_bytes(),
                                        bench._bench_capture_bytes() + width * height * 4);
      report.captureFormat = bench._bench_format();
      if (!window.webkit?.messageHandlers?.census) {
        await fetch(`/blob?label=${encodeURIComponent(`${label}-${width}x${height}`)}`,
                    { method: "POST", body: pixels }).catch(() => {});
      }
    }
    await post(report);
  }
} catch (error) {
  status.textContent = `Failed: ${error.message ?? error}`;
  await post({ error: String(error.message ?? error) });
}
