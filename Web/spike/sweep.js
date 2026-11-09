// The STEP sweep: imports a list of files one after another through the viewer's own importer
// worker, with its supervisor, warm OCCT pool and recycling as the viewer uses them, and keeps each
// cache file it makes, for the native tools to check against OCCT (Tools/web_step_sweep.py).
//
// sweep.html?label=<where it runs>[&list=fixtures/sweep/list.txt&workers=N&from=0&count=N]
//
// The list names files under /fixtures/, one a line. Each cache file is posted to /blob as
// <label>-p7-sweep-<line>; the report lists every file's outcome and time, posted as it goes and
// once more at the end.

const params = new URLSearchParams(location.search);
const listUrl = params.get("list") ?? "fixtures/sweep/list.txt";
const workers = Math.max(1, Number(params.get("workers") ??
                                   Math.max(2, Math.min(12, (navigator.hardwareConcurrency ?? 4) - 2))));
const from = Number(params.get("from") ?? 0);
const label = `${params.get("label") ?? "unlabelled"}-p7-sweep`;
const status = document.getElementById("status");
// Longer than any file in the sample takes, so only a hang reaches it.
const fileTimeoutMs = Number(params.get("timeout") ?? 600_000);

async function post(report) {
  const body = JSON.stringify({ label, ...report });
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) {
    shell.postMessage(body);
    return;
  }
  await fetch("/report", { method: "POST", body }).catch(() => {});
}

async function library() {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle("library", { create: true });
}

let importer = null;

function importOne(file, name) {
  importer ??= new Worker("/viewer/import-worker.js", { type: "module" });
  let progress = null; // the last the importer said, for a file that never finishes
  const answered = new Promise((resolve) => {
    importer.onmessage = ({ data }) => {
      if (data.type === "progress") progress = { done: data.done, total: data.total };
      if (data.type === "done" || data.type === "failed") resolve(data);
    };
    importer.onerror = (event) => resolve({ type: "failed", message: event.message ?? "the importer failed" });
    // As the viewer does: the long-lived importer's supervisor runs without threads, whose memory
    // and workers would outlive each import.
    importer.postMessage({ type: "import", file, kind: "step", name, workers, threads: false,
                           progressive: false });
  });
  let timer = 0;
  const timedOut = new Promise((resolve) => {
    timer = setTimeout(() => resolve({ type: "failed", message: `timed out at ${JSON.stringify(progress)}`,
                                       timedOut: true }), fileTimeoutMs);
  });
  return Promise.race([answered, timedOut]).then((answer) => {
    clearTimeout(timer);
    if (answer.timedOut) {
      // Its pool may be stuck: the next file gets a fresh importer.
      importer.terminate();
      importer = null;
    }
    return answer;
  });
}

// The library keeps each import's cache file, note and source; once read they go, since the sample
// would otherwise fill the origin's storage. The source is written after the import ends, so what
// cannot go yet is tried again later.
const toForget = new Set();
async function forget() {
  const folder = await library();
  for (const name of [...toForget]) {
    let gone = true;
    for (const suffix of [".geoblob", ".json", ".source"]) {
      try {
        await folder.removeEntry(name + suffix);
      } catch (error) {
        if (error.name !== "NotFoundError") gone = false;
      }
    }
    if (gone) toForget.delete(name);
  }
}

async function run() {
  const lines = (await (await fetch(listUrl)).text()).split("\n").filter(Boolean);
  const count = Number(params.get("count") ?? lines.length - from);
  const results = [];
  const started = performance.now();
  for (let line = from; line < Math.min(lines.length, from + count); ++line) {
    const path = lines[line];
    const result = { line, path };
    const began = performance.now();
    try {
      const response = await fetch(`/fixtures/${path}`);
      if (!response.ok) throw new Error(`could not fetch ${path}`);
      const file = new File([await response.blob()], path.slice(path.lastIndexOf("/") + 1));
      const name = `sweep-${line}`;
      const answer = await importOne(file, name);
      result.ms = Math.round(performance.now() - began);
      if (answer.type === "done") {
        const blob = await (await (await library()).getFileHandle(`${name}.geoblob`)).getFile();
        await fetch(`/blob?label=${encodeURIComponent(`${label}-${line}`)}`, { method: "POST", body: blob });
        Object.assign(result, { ok: true, triangles: answer.triangles, faces: answer.faces,
                                failedFaces: answer.failedFaces, slices: answer.slices,
                                importMs: answer.importMs, cacheBytes: answer.cacheBytes,
                                supervisorMB: answer.supervisorMB });
      } else {
        Object.assign(result, { ok: false, error: answer.message });
      }
      toForget.add(name);
      await forget();
    } catch (error) {
      Object.assign(result, { ok: false, error: String(error.message ?? error) });
    }
    results.push(result);
    status.textContent = `${results.length} of ${count}: ${path}\n${results.filter((r) => !r.ok).length} failed`;
    if (results.length % 50 === 0) await post({ partial: true, results });
  }
  await post({ userAgent: navigator.userAgent, workers, crossOriginIsolated: self.crossOriginIsolated,
               seconds: Math.round((performance.now() - started) / 1000), results });
  status.textContent += "\ndone";
}

run().catch((error) => post({ error: String(error.message ?? error) }));
