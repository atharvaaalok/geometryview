// The gallery's picture worker: the engine's WebGPU renderer with a GPU device of its own
// (geometryview-thumbs-web, Engine/browser/web_thumbs.cpp), drawing cache files one at a time as
// the Mac's thumbnailer does, so pictures never hold up the page's own frames.
//
// Messages in:  { type: "draw", id, blob, pixels, style, colorMode, pixelsPerPoint, lineScale,
//                 firstColor }, blob a cache file as a Blob or ArrayBuffer
//               { type: "trim" }, between pictures: lets go of the last file drawn
// Messages out: { type: "ready" } or { type: "failed", message }, once
//               { type: "drawn", id, bitmap, png, width, height, worldPerPixel, partRect, inkRect,
//                 heapMB }, the bitmap to show at once and the PNG to keep; or
//               { type: "drawn", id, message } if it could not be drawn
//
// Its memory never shrinks, so the page replaces it once heapMB passes a limit.

import createThumbs from "./build/geometryview-thumbs-web.mjs";

let module = null;
let answer = null; // the draw waiting for thumbsHost.drawn

const host = {
  ready() {
    self.postMessage({ type: "ready" });
  },
  failed(message) {
    self.postMessage({ type: "failed", message });
  },
  drawn(pixels, width, height, worldPerPixel, rects, message) {
    const resolve = answer;
    answer = null;
    resolve?.({ pixels, width, height, worldPerPixel, rects, message });
  },
};

async function draw(data) {
  const bytes = new Uint8Array(data.blob instanceof Blob ? await data.blob.arrayBuffer() : data.blob);
  // Each call may grow the heap, which detaches views made before it: read them after.
  const staging = module._thumbs_staging(bytes.length);
  module.HEAPU8.set(bytes, staging);
  const drawn = await new Promise((resolve) => {
    answer = resolve;
    module._thumbs_draw(data.pixels, data.style, data.colorMode, data.pixelsPerPoint ?? 2,
                        data.lineScale ?? 0.5, data.firstColor ?? -1);
  });
  const heapMB = Math.round(module._thumbs_heap_bytes() / 2 ** 20);
  if (!drawn.pixels) {
    self.postMessage({ type: "drawn", id: data.id, message: drawn.message || "This picture could not be drawn.", heapMB });
    return;
  }
  const { width, height } = drawn;
  const canvas = new OffscreenCanvas(width, height);
  canvas.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(drawn.pixels.buffer), width, height), 0, 0);
  const png = await canvas.convertToBlob({ type: "image/png" });
  const bitmap = canvas.transferToImageBitmap();
  self.postMessage({ type: "drawn", id: data.id, bitmap, png, width, height, worldPerPixel: drawn.worldPerPixel,
                     partRect: drawn.rects.slice(0, 4), inkRect: drawn.rects.slice(4, 8), heapMB }, [bitmap]);
}

// One message at a time, in order, so a draw never starts while another waits for the GPU.
let queue = Promise.resolve();
self.onmessage = ({ data }) => {
  queue = queue.then(async () => {
    try {
      if (data.type === "draw") await draw(data);
      else if (data.type === "trim") module?._thumbs_trim();
    } catch (error) {
      if (data.type === "draw") self.postMessage({ type: "drawn", id: data.id, message: String(error.message ?? error) });
    }
  });
};

createThumbs({ thumbsHost: host }).then((created) => {
  module = created;
  module._thumbs_start();
}, (error) => host.failed(String(error.message ?? error)));
