// One OCCT worker: meshes the STEP slices the importer sends, one at a time, with the module the
// importer compiled once and shared, and answers with OCCT's timings and the triangles it made.

import createOcct from "./occt/geometry-occt-web.mjs";

let occt = null;

self.onmessage = async (event) => {
  const message = event.data;
  if (message.module) {
    // A worker that cannot start says so, rather than leave the importer waiting for it.
    try {
      occt = await createOcct({
        // Instantiated from the importer's compiled module, so no worker compiles it again.
        instantiateWasm(imports, done) {
          WebAssembly.instantiate(message.module, imports).then((instance) => done(instance));
          return {};
        },
      });
      occt._occt_init();
      self.postMessage({ ready: true });
    } catch (error) {
      self.postMessage({ ready: false, error: String(error) });
    }
    return;
  }
  const { id, text, linear, angular, minSize } = message;
  try {
    const bytes = new Uint8Array(text);
    const pointer = occt._malloc(bytes.length);
    if (pointer === 0) throw new Error("out of memory");
    occt.HEAPU8.set(bytes, pointer);
    occt._occt_mesh(pointer, bytes.length, linear, angular, minSize);
    occt._free(pointer);
    const stats = new Float64Array(occt.HEAPF64.buffer, occt._occt_stats(), 11);
    const payload = occt.HEAPU8.slice(occt._occt_payload(), occt._occt_payload() + stats[3]);
    self.postMessage({
      id, status: stats[0], faces: stats[1], unmeshed: stats[2], read: stats[7], transfer: stats[8],
      mesh: stats[9], unmapped: stats[10], heapMB: occt.HEAPU8.length / 2 ** 20, payload: payload.buffer,
    }, [payload.buffer]);
  } catch (error) {
    // A trap, or a C++ exception OCCT did not catch: this worker can no longer be trusted.
    self.postMessage({ id, crashed: true, error: String(error) });
  }
};
