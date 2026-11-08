// The import spike: replays STEP slices saved from the native importer through OCCT compiled to
// WebAssembly, in Web Workers. With one worker it times each slice for comparison with native;
// with several it times the import's OCCT phase as the browser would run it.
//
// import.html?model=ple120&workers=4&label=<where it runs>

const params = new URLSearchParams(location.search);
const model = params.get("model") ?? "ple120";
const workerCount = Math.max(1, Number(params.get("workers") ?? 1));
// Passes over the slices. The engine compiles hot code better as it runs, so a second pass
// measures warmed-up workers, as a long import or the next file would have them.
const passes = Math.max(1, Number(params.get("passes") ?? 1));
const label = `${params.get("label") ?? "unlabelled"}-import-${model}-w${workerCount}` +
              `${passes > 1 ? `-p${passes}` : ""}`;
const status = document.getElementById("status");

async function post(report) {
  const body = JSON.stringify({ label, ...report });
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) {
    shell.postMessage(body);
    return;
  }
  await fetch("/report", { method: "POST", body }).catch(() => {});
}

try {
  // Every slice in memory first, so fetching is not part of the timing.
  status.textContent = `Loading ${model}'s slices…`;
  const slices = await (await fetch(`fixtures/slices/${model}/index.json`)).json();
  await Promise.all(slices.map(async (slice) => {
    slice.text = await (await fetch(`fixtures/slices/${model}/${slice.file}`)).arrayBuffer();
  }));

  status.textContent = "Compiling OCCT…";
  let started = performance.now();
  const module = await WebAssembly.compileStreaming(fetch("occt/geometry-occt-web.wasm"));
  const compileMs = performance.now() - started;

  started = performance.now();
  const workers = await Promise.all(Array.from({ length: workerCount }, () => new Promise(
    (resolve, reject) => {
      const worker = new Worker("occt-worker.js", { type: "module" });
      worker.onmessage = (event) => resolve({ worker, instantiateMs: event.data.instantiateMs });
      worker.onerror = (event) => reject(new Error(event.message ?? "worker failed"));
      worker.postMessage({ module });
    })));
  const startupMs = performance.now() - started;

  const results = new Array(slices.length);
  const passWallMs = [];
  let peakHeapMB = 0;
  let wallMs = 0;
  for (let pass = 0; pass < passes; ++pass) {
  status.textContent = `Meshing ${slices.length} slices in ${workerCount} worker(s), pass ${pass + 1}…`;
  let next = 0;
  started = performance.now();
  await Promise.all(workers.map(({ worker }) => new Promise((resolve) => {
    const send = () => {
      if (next >= slices.length) {
        resolve();
        return;
      }
      const id = next++;
      const slice = slices[id];
      worker.postMessage({ id, text: slice.text.slice(0), linear: slice.linear,
                           angular: slice.angular, minSize: slice.minSize });
    };
    worker.onmessage = (event) => {
      const { payload, ...result } = event.data;
      results[result.id] = { file: slices[result.id].file, bytes: slices[result.id].bytes,
                             ...result };
      peakHeapMB = Math.max(peakHeapMB, result.heapMB);
      send();
    };
    send();
  })));
  wallMs = performance.now() - started;
  passWallMs.push(Math.round(wallMs));
  }
  workers.forEach(({ worker }) => worker.terminate());

  const sum = (key) => results.reduce((total, r) => total + r[key], 0);
  const report = {
    userAgent: navigator.userAgent, model, workers: workerCount, slices: slices.length,
    compileMs: Math.round(compileMs), startupMs: Math.round(startupMs),
    instantiateMs: workers.map((w) => Math.round(w.instantiateMs)),
    wallMs: Math.round(wallMs), passWallMs, peakHeapMB: Math.round(peakHeapMB),
    seconds: { read: +sum("read").toFixed(3), transfer: +sum("transfer").toFixed(3),
               mesh: +sum("mesh").toFixed(3), calls: +(sum("wallMs") / 1000).toFixed(3) },
    failed: results.filter((r) => r.status !== 0).length,
    faces: sum("faces"), triangles: sum("triangles"), area: sum("area"),
    perSlice: results.map((r) => ({ file: r.file, status: r.status, faces: r.faces,
                                    triangles: r.triangles, area: r.area, read: r.read,
                                    transfer: r.transfer, mesh: r.mesh })),
  };
  status.textContent = JSON.stringify({ ...report, perSlice: undefined }, null, 1);
  await post(report);
} catch (error) {
  status.textContent = `Failed: ${error.message ?? error}`;
  await post({ error: String(error.message ?? error) });
}
