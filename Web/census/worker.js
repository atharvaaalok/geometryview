// The census's work off the main thread: private file storage throughput, and whether WebGPU and
// frame callbacks work in a worker with an OffscreenCanvas.

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

self.onmessage = async (event) => {
  const { task, canvas } = event.data;
  try {
    self.postMessage(task === "opfs" ? await opfs() : await gpu(canvas));
  } catch (error) {
    self.postMessage({ error: String(error) });
  }
};
