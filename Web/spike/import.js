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
// Fault injection, to time recovering from a worker that traps or runs out of memory.
const faultKind = params.get("fault");
const faultEvery = faultKind ? Math.max(1, Number(params.get("faultEvery") ?? 25)) : 0;
const label = `${params.get("label") ?? "unlabelled"}-import-${model}-w${workerCount}` +
              `${passes > 1 ? `-p${passes}` : ""}${faultKind ? `-${faultKind}` : ""}`;
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

  // A worker, ready to mesh: made from the module compiled once above.
  const spawn = () => new Promise((resolve, reject) => {
    const begun = performance.now();
    const worker = new Worker("occt-worker.js", { type: "module" });
    worker.onmessage = () => resolve({ worker, readyMs: performance.now() - begun });
    worker.onerror = (event) => reject(new Error(event.message ?? "worker failed"));
    worker.postMessage({ module });
  });
  // One request to a worker, answered by its next message.
  const ask = (worker, message, transfer) => new Promise((resolve) => {
    worker.onmessage = (event) => resolve(event.data);
    worker.onerror = (event) => resolve({ crashed: true, error: event.message ?? "error" });
    worker.postMessage(message, transfer);
  });

  started = performance.now();
  const slots = await Promise.all(Array.from({ length: workerCount }, spawn));
  const startupMs = performance.now() - started;

  const results = new Array(slices.length);
  const passWallMs = [];
  const recoveries = [];
  let peakHeapMB = 0;
  let wallMs = 0;
  let dispatched = 0;
  for (let pass = 0; pass < passes; ++pass) {
    status.textContent = `Meshing ${slices.length} slices in ${workerCount} worker(s), pass ${pass + 1}…`;
    let next = 0;
    started = performance.now();
    await Promise.all(slots.map(async (slot) => {
      while (next < slices.length) {
        const id = next++;
        const slice = slices[id];
        // With fault=trap or fault=exhaust, every faultEvery-th request makes its worker trap,
        // or run out of memory, before meshing: as a crash in OCCT would.
        const fault = faultEvery > 0 && ++dispatched % faultEvery === 0 ? faultKind : null;
        for (let attempt = 0; ; ++attempt) {
          const text = slice.text.slice(0);
          const answer = await ask(slot.worker, {
            id, text, linear: slice.linear, angular: slice.angular, minSize: slice.minSize,
            fault: attempt === 0 ? fault : null,
          }, [text]);
          if (!answer.crashed) {
            const { payload, ...result } = answer;
            results[id] = { file: slice.file, bytes: slice.bytes, ...result };
            peakHeapMB = Math.max(peakHeapMB, result.heapMB);
            break;
          }
          // Its memory may be corrupt, or full and never to shrink: replace it, then retry.
          const noticed = performance.now();
          slot.worker.terminate();
          Object.assign(slot, await spawn());
          recoveries.push({ id, fault: answer.fault, error: answer.error,
                            grantedMB: answer.grantedMB,
                            recoveryMs: +(performance.now() - noticed).toFixed(1) });
        }
      }
    }));
    wallMs = performance.now() - started;
    passWallMs.push(Math.round(wallMs));
  }
  slots.forEach(({ worker }) => worker.terminate());
  const instantiated = slots.map((slot) => slot.readyMs);

  const sum = (key) => results.reduce((total, r) => total + r[key], 0);
  const report = {
    userAgent: navigator.userAgent, model, workers: workerCount, slices: slices.length,
    compileMs: Math.round(compileMs), startupMs: Math.round(startupMs),
    instantiateMs: instantiated.map(Math.round),
    wallMs: Math.round(wallMs), passWallMs, peakHeapMB: Math.round(peakHeapMB),
    seconds: { read: +sum("read").toFixed(3), transfer: +sum("transfer").toFixed(3),
               mesh: +sum("mesh").toFixed(3), calls: +(sum("wallMs") / 1000).toFixed(3) },
    failed: results.filter((r) => r.status !== 0).length,
    recoveries, recoveryMs: recoveries.length === 0 ? null : {
      max: Math.max(...recoveries.map((r) => r.recoveryMs)),
      median: recoveries.map((r) => r.recoveryMs).sort((a, b) => a - b)[recoveries.length >> 1],
    },
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
