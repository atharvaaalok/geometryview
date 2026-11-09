// Imports STEP and STL files off the page's thread, into cache files kept in the library.
//
// Each import gets a fresh supervisor (the portable StepImport as WebAssembly), whose memory goes
// with it, since WebAssembly memory never shrinks. STEP slices go to a warm pool of OCCT workers
// that outlives imports: a worker that traps, runs out of memory, takes too long or grows past
// kRecycleMB is replaced, as in P2. The finished cache file is written to the origin-private file
// system, where the page opens it, streamed, like any other.
//
// An STL file needs no OCCT. Where the page is cross-origin isolated its packer runs on threads,
// in a worker of its own that the page ends afterwards, since a threaded module's memory is kept
// by its threads until they end.
//
// A STEP file's source is kept beside its cache, so its definitions can be imported again, finer,
// when the view comes close: a refinement, whose segments the page adds to the open document.
//
// Messages in:  { type: "import", file, kind: "step" | "stl", name, workers, threads, progressive,
//                 workerMemoryMB }
//               { type: "refine", file, level, definitions, workers, workerMemoryMB }
// Messages out: { type: "progress", done, total } and, if progressive or refining,
//               { type: "segment", bytes } as each is written; then { type: "done", ... } or
//               { type: "failed", message }

import createStepImporter from "./occt/geometry-step-web.mjs";

async function supervisor(threads) {
  if (!threads) return createStepImporter();
  const { default: create } = await import("./occt-threads/geometry-step-web.mjs");
  return create();
}

const kRecycleMB = 256;
const kTimeoutMs = 120_000;

let occtModule = null;
const pool = [];
// How large each OCCT worker's memory may grow, as the page says: small on a phone, so a worker
// that runs out fails by itself rather than taking the page past what iOS allows.
let workerMemoryMB = 0;

function spawn() {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./occt-worker.js", import.meta.url), { type: "module" });
    worker.onmessage = () => resolve({ worker, heapMB: 0, job: null, timer: 0 });
    worker.onerror = (event) => reject(new Error(event.message ?? "An OCCT worker failed to start."));
    worker.postMessage({ module: occtModule, memoryMB: workerMemoryMB });
  });
}

async function warm(size) {
  occtModule ??= await WebAssembly.compileStreaming(fetch(new URL("./occt/geometry-occt-web.wasm", import.meta.url)));
  while (pool.length < size) pool.push(await spawn());
}

// Posts each segment the importer has written since the last call, so the page shows the model
// while it imports.
function postSegments(step) {
  const place = step._malloc(16);
  while (step._step_take_segment(place)) {
    const [offset, size] = new Float64Array(step.HEAPF64.buffer, place, 2);
    const pointer = step._step_read(offset, size);
    if (pointer === 0) break;
    const bytes = step.HEAPU8.slice(pointer, pointer + size);
    self.postMessage({ type: "segment", bytes }, [bytes.buffer]);
  }
  step._free(place);
}

// Hands the supervisor's slices to the pool until every one is answered.
function mesh(step, slots, progressive) {
  const total = step._step_jobs();
  let replies = 0;
  const replacing = new Set();
  return new Promise((resolve, reject) => {
    const idle = [...slots];
    let busy = 0;
    let closed = false;

    const pump = () => {
      while (!closed && idle.length > 0) {
        const id = step._step_next();
        if (id === -2) {
          if (busy === 0) {
            closed = true;
            Promise.all(replacing).then(() => resolve(replies));
          }
          return;
        }
        if (id === -1) {
          if (busy === 0) reject(new Error("The import stalled with slices unanswered."));
          return;
        }
        const text = step.HEAPU8.slice(step._step_text(), step._step_text() + step._step_text_size());
        const [linear, angular, minSize] = new Float64Array(step.HEAPF64.buffer, step._step_request(), 3);
        const slot = idle.pop();
        slot.job = id;
        busy += 1;
        slot.worker.onmessage = (event) => answered(slot, event.data);
        slot.worker.onerror = (event) => answered(slot, { crashed: true, error: event.message });
        slot.timer = setTimeout(() => answered(slot, { crashed: true, error: "timed out" }), kTimeoutMs);
        slot.worker.postMessage({ id, text: text.buffer, linear, angular, minSize }, [text.buffer]);
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
      if (slot.job === null) return;
      clearTimeout(slot.timer);
      const id = slot.job;
      slot.job = null;
      busy -= 1;
      try {
        if (answer.crashed) {
          step._step_crash(id);
          pump();
          await replace(slot);
        } else {
          // Each call may grow the heap, which detaches views made before it: read them after.
          const payload = new Uint8Array(answer.payload);
          const pointer = step._step_payload(payload.length);
          step.HEAPU8.set(payload, pointer);
          step._step_reply(id, answer.status, answer.faces, answer.unmapped, answer.unmeshed,
                           answer.read, answer.transfer, answer.mesh);
          if (progressive) postSegments(step);
          replies += 1;
          slot.heapMB = answer.heapMB;
          if (slot.heapMB > kRecycleMB) await replace(slot);
          if (replies % 8 === 0) self.postMessage({ type: "progress", done: replies, total });
        }
        idle.push(slot);
        pump();
      } catch (error) {
        reject(error);
      }
    };
    pump();
  });
}

async function library() {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle("library", { create: true });
}

async function openForWriting(folder, fileName) {
  const access = await (await folder.getFileHandle(fileName, { create: true })).createSyncAccessHandle();
  access.truncate(0);
  return access;
}

// Writes the cache file to the library, with a note of the file it came from.
async function keep(name, bytes, note) {
  const folder = await library();
  const write = async (fileName, data) => {
    const access = await openForWriting(folder, fileName);
    for (let at = 0; at < data.length;) at += access.write(data.subarray(at), { at });
    access.flush();
    access.close();
  };
  await write(`${name}.geoblob`, bytes);
  await write(`${name}.json`, new TextEncoder().encode(JSON.stringify(note)));
}

// Keeps a STEP file beside its cache, read again in pieces rather than held through the import,
// once the import is done, since nothing waits for it. The library uses it only when its size
// matches the note's, so a copy cut short is never read.
async function keepSource(name, file) {
  const access = await openForWriting(await library(), `${name}.source`);
  for (let at = 0; at < file.size; at += kSourcePiece) {
    const piece = new Uint8Array(await file.slice(at, at + kSourcePiece).arrayBuffer());
    for (let done = 0; done < piece.length;) done += access.write(piece.subarray(done), { at: at + done });
  }
  access.flush();
  access.close();
}
const kSourcePiece = 8 << 20;

// Imports some of a STEP file's definitions again at a finer level, posting each segment as it is
// written; they hold geometry only, for the page to add to the document already open.
async function refine(data) {
  try {
    const step = await supervisor(false);
    const bytes = new Uint8Array(await data.file.arrayBuffer());
    // Each call may grow the heap, which detaches views made before it: read them after.
    const pointer = step._step_open(bytes.length);
    step.HEAPU8.set(bytes, pointer);
    const room = step._step_refine(data.level, data.definitions.length);
    new Uint32Array(step.HEAPU8.buffer, room, data.definitions.length).set(data.definitions);
    if (step._step_begin() !== 0) throw new Error(step.UTF8ToString(step._step_error()));
    await warm(data.workers);
    await mesh(step, pool, true);
    if (step._step_finish() !== 0) throw new Error(step.UTF8ToString(step._step_error()));
    postSegments(step);
    // What it made, for the page's scripted check: a definition can mesh to nothing at all.
    const stats = new Float64Array(step.HEAPF64.buffer, step._step_stats(), 16);
    self.postMessage({ type: "done", triangles: stats[11], faces: stats[10], failedFaces: stats[12],
                       crashedWorkers: stats[14] });
  } catch (error) {
    self.postMessage({ type: "failed", message: String(error.message ?? error) });
  }
}

// Hands an STL file to the importer a piece at a time. False if it must be read whole instead,
// which the importer says on seeing the first piece.
async function readPieces(step, file) {
  const room = step._stl_open(file.size);
  const piece = step._stl_piece_size();
  for (let at = 0; at < file.size; at += piece) {
    const bytes = new Uint8Array(await file.slice(at, Math.min(file.size, at + piece)).arrayBuffer());
    step.HEAPU8.set(bytes, room);
    if (step._stl_take(bytes.length) === 0) return false;
  }
  return file.size > 0;
}

self.onmessage = async ({ data }) => {
  workerMemoryMB = data.workerMemoryMB ?? workerMemoryMB;
  if (data.type === "refine") {
    await refine(data);
    return;
  }
  if (data.type !== "import") return;
  const started = performance.now();
  try {
    const step = await supervisor(data.threads && self.crossOriginIsolated);
    // A binary STL file is read in pieces, so it is never held whole; anything else, whole.
    const inPieces = data.kind === "stl" && await readPieces(step, data.file);
    if (!inPieces) {
      const bytes = new Uint8Array(await data.file.arrayBuffer());
      const pointer = step._step_open(bytes.length);
      step.HEAPU8.set(bytes, pointer);
    }
    let slices = 0;
    if (data.kind === "stl") {
      const made = inPieces ? step._mesh_import_pieces() : step._mesh_import();
      if (made !== 0) throw new Error(step.UTF8ToString(step._step_error()));
    } else {
      if (step._step_begin() !== 0) throw new Error(step.UTF8ToString(step._step_error()));
      self.postMessage({ type: "progress", done: 0, total: step._step_jobs() });
      if (data.progressive) postSegments(step); // the model, written by step_begin
      await warm(data.workers);
      slices = await mesh(step, pool, data.progressive);
      if (step._step_finish() !== 0) throw new Error(step.UTF8ToString(step._step_error()));
      if (data.progressive) postSegments(step);
    }
    const blob = step.HEAPU8.subarray(step._step_blob(), step._step_blob() + step._step_blob_size());
    const stats = new Float64Array(step.HEAPF64.buffer, step._step_stats(), 16);
    const result = { triangles: stats[11], faces: stats[10], failedFaces: stats[12], slices,
                     importMs: Math.round(performance.now() - started), cacheBytes: blob.length,
                     supervisorMB: Math.round(step.HEAPU8.length / 2 ** 20) };
    await keep(data.name, blob, { name: data.name, size: data.file.size,
                                  lastModified: data.file.lastModified, importedAt: Date.now(), ...result });
    self.postMessage({ type: "done", ...result });
    if (data.kind === "step") await keepSource(data.name, data.file).catch(() => {});
  } catch (error) {
    self.postMessage({ type: "failed", message: String(error.message ?? error) });
  }
};
