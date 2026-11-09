// A whole STEP import in the browser: the supervisor (the portable StepImport, compiled to
// WebAssembly) indexes the file and hands out its slices, and a warm pool of OCCT Web Workers
// meshes them. The cache file it makes is the one the Mac's importer makes.
//
// step.html?model=hard&workers=12&label=<where it runs>
//   passes     imports the file this many times on the same pool (default 1); after the first,
//              the workers' code is already compiled, as for every file after the first in a session
//   recycleMB  a worker whose heap has grown past this is replaced after its reply (default 256),
//              since WebAssembly memory never shrinks
//   timeout    seconds a slice may take before its worker is replaced (default 120)
//   fault      trap or exhaust: every faultEvery-th slice (default 25) makes its worker fail first
//
// The supervisor runs on the page's own thread here; the web runtime moves it into a worker.

import createStepImporter from "./occt/geometryview-step-web.mjs";

const params = new URLSearchParams(location.search);
const model = params.get("model") ?? "ple120";
const workerCount = Math.max(1, Number(params.get("workers") ?? 4));
const passes = Math.max(1, Number(params.get("passes") ?? 1));
const recycleMB = Number(params.get("recycleMB") ?? 256);
const timeoutMs = Number(params.get("timeout") ?? 120) * 1000;
const faultKind = params.get("fault");
const faultEvery = faultKind ? Math.max(1, Number(params.get("faultEvery") ?? 25)) : 0;
const label = `${params.get("label") ?? "unlabelled"}-import-step-${model}-w${workerCount}` +
              `${passes > 1 ? `-p${passes}` : ""}${faultKind ? `-${faultKind}` : ""}`;
const status = document.getElementById("status");
const inShell = Boolean(window.webkit?.messageHandlers?.census);

async function post(report) {
  const body = JSON.stringify({ label, ...report });
  if (inShell) {
    window.webkit.messageHandlers.census.postMessage(body);
    return;
  }
  await fetch("/report", { method: "POST", body }).catch(() => {});
}

const ms = (value) => Math.round(value);

// One import of `file` on the pool in `slots`, from the bytes in hand to the cache file.
async function importOnce(step, file, slots, spawn) {
  status.textContent = `Reading ${model}'s structure…`;
  const importStarted = performance.now();
  // Each call may grow the heap, which detaches the views made before it: read them after.
  const filePointer = step._step_open(file.byteLength);
  step.HEAPU8.set(new Uint8Array(file), filePointer);
  const begun = step._step_begin();
  if (begun !== 0) {
    throw new Error(step.UTF8ToString(step._step_error()));
  }
  const beginMs = performance.now() - importStarted;

  status.textContent = `Meshing ${model} in ${workerCount} worker(s)…`;
  const meshStarted = performance.now();
  let dispatched = 0;
  let replies = 0;
  let peakWorkerMB = 0;
  let peakPoolMB = 0;
  const recoveries = [];
  const recycled = [];
  const replacing = new Set();
  const done = await new Promise((resolve, reject) => {
    const idle = [...slots];
    let busy = 0;
    let closed = false; // every slice answered; a worker replaced after that has nothing to do

    // Hands queued slices to idle workers until one or the other runs out.
    const pump = () => {
      while (!closed && idle.length > 0) {
        const id = step._step_next();
        if (id === -2) {
          if (busy === 0) {
            closed = true;
            resolve(true);
          }
          return;
        }
        if (id === -1) {
          // Replies still due may add retries; with none due, a slice has been lost.
          if (busy === 0) reject(new Error("The import stalled with slices unanswered."));
          return;
        }
        const text = step.HEAPU8.slice(step._step_text(), step._step_text() + step._step_text_size());
        const [linear, angular, minSize] = new Float64Array(step.HEAPF64.buffer, step._step_request(), 3);
        const fault = faultEvery > 0 && ++dispatched % faultEvery === 0 ? faultKind : null;
        const slot = idle.pop();
        slot.job = id;
        busy += 1;
        slot.worker.onmessage = (event) => answered(slot, event.data);
        slot.worker.onerror = (event) => answered(slot, { crashed: true, error: event.message });
        slot.timer = setTimeout(() => answered(slot, { crashed: true, error: "timed out" }), timeoutMs);
        slot.worker.postMessage({ id, text: text.buffer, linear, angular, minSize, fault },
                                [text.buffer]);
      }
    };

    // Its memory may be corrupt, or full and never to shrink: a fresh worker takes its place.
    const replace = (slot) => {
      slot.worker.terminate();
      const replaced = spawn().then((fresh) => Object.assign(slot, fresh));
      replacing.add(replaced);
      return replaced.finally(() => replacing.delete(replaced));
    };

    const answered = async (slot, answer) => {
      if (slot.job === null) return; // a late message from a worker already given up on
      clearTimeout(slot.timer);
      const id = slot.job;
      slot.job = null;
      busy -= 1;
      try {
        if (answer.crashed) {
          // The slice goes down the failure ladder at once, to the workers that are free.
          step._step_crash(id);
          pump();
          const noticed = performance.now();
          await replace(slot);
          recoveries.push({ id, fault: answer.fault ?? null, error: answer.error,
                            recoveryMs: +(performance.now() - noticed).toFixed(1) });
        } else {
          const payload = new Uint8Array(answer.payload);
          const payloadPointer = step._step_payload(payload.length);
          step.HEAPU8.set(payload, payloadPointer);
          step._step_reply(id, answer.status, answer.faces, answer.unmapped, answer.unmeshed,
                           answer.read, answer.transfer, answer.mesh);
          replies += 1;
          slot.heapMB = answer.heapMB;
          peakWorkerMB = Math.max(peakWorkerMB, slot.heapMB);
          peakPoolMB = Math.max(peakPoolMB, slots.reduce((sum, s) => sum + s.heapMB, 0));
          if (slot.heapMB > recycleMB) {
            recycled.push(Math.round(slot.heapMB));
            await replace(slot);
          }
        }
        idle.push(slot);
        pump();
        if (replies % 50 === 0) {
          status.textContent = `Meshing ${model} in ${workerCount} worker(s): ${replies} slices…`;
        }
      } catch (error) {
        reject(error);
      }
    };

    pump();
  });
  const meshMs = performance.now() - meshStarted;
  await Promise.all(replacing); // workers still being replaced when the last slice came back

  const finishStarted = performance.now();
  if (!done || step._step_finish() !== 0) {
    throw new Error(step.UTF8ToString(step._step_error()));
  }
  const finishMs = performance.now() - finishStarted;
  const importMs = performance.now() - importStarted;

  const blob = step.HEAPU8.slice(step._step_blob(), step._step_blob() + step._step_blob_size());
  const v = new Float64Array(step.HEAPF64.buffer, step._step_stats(), 17);
  const report = {
    importMs: ms(importMs), beginMs: ms(beginMs), meshMs: ms(meshMs), finishMs: ms(finishMs),
    seconds: {
      tokenize: +v[0].toFixed(3), structure: +v[1].toFixed(3), definitions: +v[2].toFixed(3),
      read: +v[3].toFixed(3), transfer: +v[4].toFixed(3), mesh: +v[5].toFixed(3),
      pack: +v[6].toFixed(3),
    },
    records: v[7], bodies: v[8], definitions: v[9], faces: v[10], triangles: v[11],
    failedFaces: v[12], unmappedFaces: v[13], crashedWorkers: v[14], meshlets: v[15],
    coarserFaces: v[16],
    slices: replies, recoveries, recycled,
    memoryMB: { supervisor: ms(step.HEAPU8.length / 2 ** 20), peakWorker: ms(peakWorkerMB),
                peakPool: ms(peakPoolMB) },
    blobBytes: blob.length,
  };
  return { report, blob };
}

try {
  let started = performance.now();
  status.textContent = `Loading ${model}…`;
  // The file, the supervisor and the OCCT module all load at once, as the app's start would.
  let [file, step, occtModule] = await Promise.all([
    fetch(`fixtures/step/${model}.step`).then((response) => response.arrayBuffer()),
    createStepImporter(),
    WebAssembly.compileStreaming(fetch("occt/geometryview-occt-web.wasm")),
  ]);
  const loadMs = performance.now() - started;

  // The warm pool: workers made once from the module compiled above, ready before a file opens.
  const spawn = () => new Promise((resolve, reject) => {
    const worker = new Worker("occt-worker.js", { type: "module" });
    worker.onmessage = () => resolve({ worker, heapMB: 0, job: null, timer: 0 });
    worker.onerror = (event) => reject(new Error(event.message ?? "worker failed"));
    worker.postMessage({ module: occtModule });
  });
  started = performance.now();
  const slots = await Promise.all(Array.from({ length: workerCount }, spawn));
  const poolMs = performance.now() - started;

  // The first pass is a session's first file; later ones find the pool's code compiled.
  let first = null;
  let blob = null;
  const later = [];
  for (let pass = 0; pass < passes; ++pass) {
    const result = await importOnce(step, file, slots, spawn);
    blob = result.blob;
    if (first === null) {
      first = result.report;
    } else {
      const { importMs, beginMs, meshMs, finishMs, recoveries } = result.report;
      later.push({ importMs, beginMs, meshMs, finishMs, recoveries: recoveries.length });
    }
  }
  file = null;
  slots.forEach(({ worker }) => worker.terminate());

  const report = {
    userAgent: navigator.userAgent, model, workers: workerCount, recycleMB,
    crossOriginIsolated: self.crossOriginIsolated, loadMs: ms(loadMs), poolMs: ms(poolMs),
    ...first, later,
  };
  status.textContent = JSON.stringify({ ...report, recoveries: report.recoveries.length }, null, 1);
  if (!inShell) {
    await fetch(`/blob?label=${encodeURIComponent(label)}`, { method: "POST", body: blob })
      .catch(() => {});
  }
  await post(report);
} catch (error) {
  status.textContent = `Failed: ${error.message ?? error}`;
  await post({ error: String(error.message ?? error) });
}
