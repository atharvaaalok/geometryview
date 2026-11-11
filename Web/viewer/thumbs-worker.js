// The gallery's picture worker: the engine's WebGPU renderer with a GPU device of its own
// (geometryview-thumbs-web, Engine/browser/web_thumbs.cpp), drawing cache files one at a time as
// the Mac's thumbnailer does, so pictures never hold up the page's own frames.
//
// Messages in:  { type: "draw", id, blob, pixels, style, colorMode, pixelsPerPoint, lineScale,
//                 firstColor, folderColor, keep }, blob a cache file as a Blob or ArrayBuffer.
//                 A file of one part takes `folderColor`, if given. With keep { key, picture },
//                 the picture, its note and the file's facts are kept in the origin-private file
//                 system (kept.js), written here since only a worker can write there everywhere.
//               { type: "trim" }, between pictures: lets go of the last file drawn
// Messages out: { type: "ready" } or { type: "failed", message }, once
//               { type: "drawn", id, bitmap, png, width, height, worldPerPixel, partRect, inkRect,
//                 facts, firstColor, heapMB }, the bitmap to show at once and the PNG to keep; or
//               { type: "drawn", id, message } if it could not be drawn
//
// Its memory never shrinks, so the page replaces it once heapMB passes a limit.

import createThumbs from "./build/geometryview-thumbs-web.mjs";
import { keepPicture } from "./kept.js";

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
  // Its facts first: drawing lets go of the staged file.
  const factsText = module.UTF8ToString(module._thumbs_facts());
  const facts = factsText ? JSON.parse(factsText) : null;
  const firstColor = data.folderColor >= 0 && facts?.parts === 1 ? data.folderColor : data.firstColor ?? -1;
  const drawn = await new Promise((resolve) => {
    answer = resolve;
    module._thumbs_draw(data.pixels, data.style, data.colorMode, data.pixelsPerPoint ?? 2,
                        data.lineScale ?? 0.5, firstColor);
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
  const note = { width, height, mmPerPixel: drawn.worldPerPixel, part: drawn.rects.slice(0, 4),
                 ink: drawn.rects.slice(4, 8), firstColor };
  if (data.keep) await keepPicture(data.keep.key, data.keep.picture, png, note, facts).catch(() => {});
  self.postMessage({ type: "drawn", id: data.id, bitmap, png, width, height, worldPerPixel: drawn.worldPerPixel,
                     partRect: note.part, inkRect: note.ink, facts, firstColor, heapMB }, [bitmap]);
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
