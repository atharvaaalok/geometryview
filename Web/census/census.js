// Measures what this browser or app pane offers the universal viewer: WebGPU features and limits,
// WebAssembly features, threads, frame pacing, private file storage, WebGPU in a worker, and,
// when asked with ?mode=gpu-memory or ?mode=wasm-memory, how much memory it really grants.
// Each finished step is posted to the census server, so a page the system kills still reports.

const params = new URLSearchParams(location.search);
const mode = params.get("mode") ?? "core";
const report = {
  label: `${params.get("label") ?? "unlabelled"}${mode === "core" ? "" : `-${mode}`}`,
  mode,
  startedAt: new Date().toISOString(),
};
const status = document.getElementById("status");
const output = document.getElementById("report");

async function post(partial) {
  output.textContent = JSON.stringify(report, null, 2);
  const body = JSON.stringify({ ...report, partial });
  // Inside the iOS shell the app saves reports; elsewhere the census server does.
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) {
    shell.postMessage(body);
    return;
  }
  try {
    await fetch("/report", { method: "POST", body });
  } catch {
    // Opened without the census server: the page still shows the report.
  }
}

async function step(name, measure) {
  status.textContent = `Measuring ${name}…`;
  try {
    report[name] = await measure();
  } catch (error) {
    report[name] = { error: String(error) };
  }
  await post(true);
}

// ---- Environment -----------------------------------------------------------------------------

function environment() {
  return {
    userAgent: navigator.userAgent,
    platform: navigator.userAgentData?.platform ?? navigator.platform,
    hardwareConcurrency: navigator.hardwareConcurrency,
    deviceMemory: navigator.deviceMemory ?? null,
    devicePixelRatio: devicePixelRatio,
    screen: [screen.width, screen.height],
    secureContext: isSecureContext,
    crossOriginIsolated: self.crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer === "function",
    offscreenCanvas: typeof OffscreenCanvas === "function",
    storageFoundation: typeof navigator.storage?.getDirectory === "function",
  };
}

// ---- WebAssembly features, by validating the smallest module that uses each --------------------

const header = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
const probes = {
  // () -> v128: i32.const 0; i8x16.splat; i8x16.popcnt
  simd: [1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11],
  // i8x16.relaxed_swizzle
  relaxedSimd: [1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 15, 1, 13, 0, 65, 1, 253, 15, 65, 2, 253, 15,
                253, 128, 2, 11],
  // try / catch_all, the legacy exception instructions
  legacyExceptions: [1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 8, 1, 6, 0, 6, 64, 25, 11, 11],
  // try_table with no catches, the exnref instructions
  exnrefExceptions: [1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 8, 1, 6, 0, 31, 64, 0, 11, 11],
  // return_call 0
  tailCalls: [1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11],
  // a memory with 64-bit addresses
  memory64: [5, 3, 1, 4, 1],
};

function wasmFeatures() {
  const result = {};
  for (const [name, body] of Object.entries(probes)) {
    result[name] = WebAssembly.validate(new Uint8Array([...header, ...body]));
  }
  try {
    new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
    result.sharedMemory = true;
  } catch {
    result.sharedMemory = false;
  }
  result.jspi = typeof WebAssembly.Suspending === "function";
  result.streamingCompile = typeof WebAssembly.compileStreaming === "function";
  return result;
}

// ---- WebGPU ----------------------------------------------------------------------------------

const limitNames = [
  "maxTextureDimension2D", "maxBindGroups", "maxBindingsPerBindGroup",
  "maxDynamicStorageBuffersPerPipelineLayout", "maxStorageBuffersPerShaderStage",
  "maxStorageTexturesPerShaderStage", "maxUniformBuffersPerShaderStage",
  "maxUniformBufferBindingSize", "maxStorageBufferBindingSize", "maxBufferSize",
  "minStorageBufferOffsetAlignment", "maxVertexBuffers", "maxVertexAttributes",
  "maxInterStageShaderVariables", "maxColorAttachments", "maxColorAttachmentBytesPerSample",
  "maxComputeWorkgroupStorageSize", "maxComputeInvocationsPerWorkgroup",
  "maxComputeWorkgroupSizeX", "maxComputeWorkgroupsPerDimension",
];

function limitsOf(limits) {
  const result = {};
  for (const name of limitNames) {
    result[name] = limits[name];
  }
  for (const name in limits) {
    result[name] ??= limits[name];
  }
  return result;
}

let device = null;

async function webgpu() {
  if (!navigator.gpu) {
    return { available: false };
  }
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) {
    return { available: true, adapter: false };
  }
  const info = adapter.info ?? {};
  const limits = limitsOf(adapter.limits);
  device = await adapter.requestDevice({
    requiredFeatures: [...adapter.features],
    requiredLimits: {
      maxBufferSize: limits.maxBufferSize,
      maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize,
      maxStorageBuffersPerShaderStage: limits.maxStorageBuffersPerShaderStage,
      maxComputeWorkgroupStorageSize: limits.maxComputeWorkgroupStorageSize,
      maxComputeInvocationsPerWorkgroup: limits.maxComputeInvocationsPerWorkgroup,
    },
  });
  // One cleared frame proves the canvas presents.
  const context = document.getElementById("canvas").getContext("webgpu");
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "opaque" });
  const encoder = device.createCommandEncoder();
  encoder.beginRenderPass({
    colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: "clear",
                         clearValue: { r: 0.46, g: 0.63, b: 0.89, a: 1 }, storeOp: "store" }],
  }).end();
  device.queue.submit([encoder.finish()]);
  await device.queue.onSubmittedWorkDone();
  return {
    available: true,
    adapter: true,
    info: { vendor: info.vendor, architecture: info.architecture, device: info.device,
            description: info.description },
    isFallbackAdapter: adapter.isFallbackAdapter ?? info.isFallbackAdapter ?? null,
    features: [...adapter.features].sort(),
    limits,
    preferredCanvasFormat: format,
    transientAttachmentUsage: typeof GPUTextureUsage?.TRANSIENT_ATTACHMENT === "number",
    wgslLanguageFeatures: navigator.gpu.wgslLanguageFeatures
      ? [...navigator.gpu.wgslLanguageFeatures].sort() : null,
  };
}

// ---- Frame pacing ------------------------------------------------------------------------------

function framePacing() {
  return new Promise((resolve) => {
    const times = [];
    function tick(time) {
      times.push(time);
      if (times.length < 121) {
        requestAnimationFrame(tick);
        return;
      }
      const gaps = times.slice(1).map((t, i) => t - times[i]).sort((a, b) => a - b);
      const median = gaps[gaps.length >> 1];
      resolve({ medianIntervalMs: +median.toFixed(3), hz: +(1000 / median).toFixed(1),
                p95IntervalMs: +gaps[Math.floor(gaps.length * 0.95)].toFixed(3) });
    }
    requestAnimationFrame(tick);
  });
}

// ---- Worker: private file storage and WebGPU off the main thread ------------------------------

function inWorker(message, transfer = []) {
  return new Promise((resolve) => {
    const worker = new Worker("worker.js", { type: "module" });
    const timeout = setTimeout(() => resolve({ error: "timed out" }), 20000);
    worker.onmessage = (event) => {
      clearTimeout(timeout);
      worker.terminate();
      resolve(event.data);
    };
    worker.onerror = (event) => {
      clearTimeout(timeout);
      resolve({ error: event.message ?? "worker error" });
    };
    worker.postMessage(message, transfer);
  });
}

async function storage() {
  const estimate = await navigator.storage?.estimate?.();
  const opfs = await inWorker({ task: "opfs" });
  return { estimate, persisted: await navigator.storage?.persisted?.(), opfs };
}

async function workerGpu() {
  const canvas = new OffscreenCanvas(64, 64);
  return inWorker({ task: "gpu", canvas }, [canvas]);
}

// ---- Memory ceilings, on request ---------------------------------------------------------------

const MB = 1024 * 1024;

// Allocates and clears 256 MB GPU buffers until the device refuses, keeping them all alive.
async function gpuMemory() {
  const result = { stepMB: 256, grantedMB: 0, stoppedBy: null };
  report.gpuMemory = result;
  const buffers = [];
  for (let total = 0; total < 8192; total += 256) {
    device.pushErrorScope("out-of-memory");
    device.pushErrorScope("validation");
    const buffer = device.createBuffer({ size: 256 * MB, usage: GPUBufferUsage.STORAGE |
                                         GPUBufferUsage.COPY_DST });
    const encoder = device.createCommandEncoder();
    encoder.clearBuffer(buffer);
    device.queue.submit([encoder.finish()]);
    await device.queue.onSubmittedWorkDone();
    const invalid = await device.popErrorScope();
    const outOfMemory = await device.popErrorScope();
    if (invalid || outOfMemory) {
      result.stoppedBy = String((outOfMemory ?? invalid).message ?? "error");
      break;
    }
    buffers.push(buffer);
    result.grantedMB = total + 256;
    await post(true);
  }
  buffers.forEach((buffer) => buffer.destroy());
  return result;
}

// Grows one WebAssembly memory 64 MB at a time, touching every page, up to the wasm32 limit.
async function wasmMemory() {
  const result = { stepMB: 64, grantedMB: 0, stoppedBy: null };
  report.wasmMemory = result;
  const pagesPerStep = 64 * MB / 65536;
  const memory = new WebAssembly.Memory({ initial: 0, maximum: 65536 });
  for (let total = 0; total < 4096; total += 64) {
    try {
      const previous = memory.grow(pagesPerStep) * 65536;
      new Uint8Array(memory.buffer, previous, 64 * MB).fill(1);
    } catch (error) {
      result.stoppedBy = String(error);
      break;
    }
    result.grantedMB = total + 64;
    await post(true);
  }
  return result;
}

// ---- Run ----------------------------------------------------------------------------------------

await step("environment", environment);
await step("wasm", wasmFeatures);
await step("webgpu", webgpu);
await step("framePacing", framePacing);
await step("storage", storage);
await step("workerGpu", workerGpu);
if (mode === "gpu-memory" && device) {
  await step("gpuMemory", gpuMemory);
}
if (mode === "wasm-memory") {
  await step("wasmMemory", wasmMemory);
}
report.finishedAt = new Date().toISOString();
await post(false);
status.textContent = "Done. The report was saved by the census server.";
