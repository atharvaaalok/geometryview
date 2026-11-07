// The census's work off the main thread: private file storage throughput, whether WebGPU and
// frame callbacks work in a worker with an OffscreenCanvas, and how workers share WebAssembly.

const MB = 1024 * 1024;

async function opfs() {
  const root = await navigator.storage.getDirectory();
  const file = await root.getFileHandle("census-probe.bin", { create: true });
  if (typeof file.createSyncAccessHandle !== "function") {
    return { syncAccessHandle: false };
  }
  const handle = await file.createSyncAccessHandle();
  const chunk = new Uint8Array(4 * MB).fill(7);
  const total = 256 * MB;
  let start = performance.now();
  for (let offset = 0; offset < total; offset += chunk.length) {
    handle.write(chunk, { at: offset });
  }
  handle.flush();
  const writeMs = performance.now() - start;
  start = performance.now();
  for (let offset = 0; offset < total; offset += chunk.length) {
    handle.read(chunk, { at: offset });
  }
  const readMs = performance.now() - start;
  handle.close();
  await root.removeEntry("census-probe.bin");
  return {
    syncAccessHandle: true,
    bytesMB: total / MB,
    writeMBps: Math.round(total / MB / (writeMs / 1000)),
    readMBps: Math.round(total / MB / (readMs / 1000)),
  };
}

async function gpu(canvas) {
  if (!self.navigator.gpu) {
    return { available: false };
  }
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const context = canvas.getContext("webgpu");
  context.configure({ device, format: navigator.gpu.getPreferredCanvasFormat() });
  const encoder = device.createCommandEncoder();
  encoder.beginRenderPass({
    colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: "clear",
                         clearValue: { r: 1, g: 0.96, b: 0.84, a: 1 }, storeOp: "store" }],
  }).end();
  device.queue.submit([encoder.finish()]);
  await device.queue.onSubmittedWorkDone();
  const frames = typeof self.requestAnimationFrame === "function"
    ? await new Promise((resolve) => {
        let count = 0;
        const start = performance.now();
        const tick = () => {
          if (++count < 60) {
            self.requestAnimationFrame(tick);
          } else {
            resolve({ hz: +(59000 / (performance.now() - start)).toFixed(1) });
          }
        };
        self.requestAnimationFrame(tick);
      })
    : null;
  return { available: true, presented: true, workerAnimationFrame: frames };
}

// Instantiates a module the page compiled and posted here.
async function instantiate(module) {
  const start = performance.now();
  const instance = await WebAssembly.instantiate(module);
  return { works: instance.exports.add(2, 3) === 5,
           instantiateMs: +(performance.now() - start).toFixed(2) };
}

async function compile() {
  const module = await WebAssembly.compileStreaming(fetch("tiny.wasm"));
  return { works: new WebAssembly.Instance(module).exports.add(2, 3) === 5 };
}

// Takes turns with the other worker: each waits for its turn, then hands the turn over. Role 0
// times the rounds; each round is two hand-overs.
function atomics({ memory, role, rounds }) {
  const turn = new Int32Array(memory.buffer, 0, 1);
  const start = performance.now();
  for (let round = 0; round < rounds; ++round) {
    while (Atomics.load(turn, 0) !== role) {
      Atomics.wait(turn, 0, 1 - role);
    }
    Atomics.store(turn, 0, 1 - role);
    Atomics.notify(turn, 0);
  }
  const ms = performance.now() - start;
  return { rounds, handOverMicroseconds: +((ms * 1000) / (2 * rounds)).toFixed(2) };
}

const tasks = {
  opfs: () => opfs(),
  gpu: ({ canvas }) => gpu(canvas),
  instantiate: ({ module }) => instantiate(module),
  compile: () => compile(),
  atomics: (message) => atomics(message),
};

self.onmessage = async (event) => {
  try {
    self.postMessage(await tasks[event.data.task](event.data));
  } catch (error) {
    self.postMessage({ error: String(error) });
  }
};
