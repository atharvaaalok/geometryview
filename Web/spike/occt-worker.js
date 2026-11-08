// One OCCT worker: meshes the STEP slices the page sends, one at a time, with the module the page
// compiled once and shared, and answers with OCCT's own timings and what it made.

import createOcct from "./occt/geometry-occt-web.mjs";

let occt = null;

self.onmessage = async (event) => {
  const message = event.data;
  if (message.module) {
    const started = performance.now();
    occt = await createOcct({
      // The page compiled the module; instantiating it here skips compiling it per worker.
      instantiateWasm(imports, done) {
        WebAssembly.instantiate(message.module, imports).then((instance) => done(instance));
        return {};
      },
    });
    occt._occt_init();
    self.postMessage({ ready: true, instantiateMs: performance.now() - started });
    return;
  }
  const { id, text, linear, angular, minSize } = message;
  const bytes = new Uint8Array(text);
  const pointer = occt._malloc(bytes.length);
  occt.HEAPU8.set(bytes, pointer);
  const started = performance.now();
  const ok = occt._occt_mesh(pointer, bytes.length, linear, angular, minSize);
  const wallMs = performance.now() - started;
  occt._free(pointer);
  const stats = new Float64Array(occt.HEAPF64.buffer, occt._occt_stats(), 10);
  // The payload goes back as the native worker's does, so its copy is part of the cost.
  const payload = occt.HEAPU8.slice(occt._occt_payload(), occt._occt_payload() + stats[3]);
  self.postMessage({
    id, ok, wallMs, status: stats[0], faces: stats[1], unmeshed: stats[2], payloadBytes: stats[3],
    triangles: stats[4], area: stats[5], volume: stats[6], read: stats[7], transfer: stats[8],
    mesh: stats[9], heapMB: occt.HEAPU8.length / 2 ** 20, payload: payload.buffer,
  }, [payload.buffer]);
};
