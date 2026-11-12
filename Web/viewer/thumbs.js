// Draws the gallery's pictures in a worker of their own (thumbs-worker.js), with a GPU device of
// its own, one at a time. WebAssembly memory never shrinks, so once the worker's heap passes
// kRecycleMB after a picture it is replaced, as the import worker replaces its OCCT workers; and
// after a moment with nothing to draw it lets go of the last file it drew.

export const kRecycleMB = 256;
const kTrimAfterMs = 2000;

export class Thumbnailer {
  #url;
  #recycleMB;
  #worker = null;
  #ready = null;
  #answers = new Map(); // id -> resolve
  #next = 1;
  #queue = Promise.resolve();
  #trimTimer = 0;
  /** Workers replaced as their memory grew, and the heap of the last picture, for checks. */
  recycled = 0;
  heapMB = 0;

  constructor({ url = new URL("./thumbs-worker.js", import.meta.url), recycleMB = kRecycleMB } = {}) {
    this.#url = url;
    this.#recycleMB = recycleMB;
  }

  /**
   * Draws a cache file (a Blob or ArrayBuffer) as spec says: { pixels, style, colorMode,
   * pixelsPerPoint, lineScale }, its parts starting at firstColor (-1 for the cycle's own), or a
   * file of one part at folderColor. With keep { key, picture }, the picture and the file's facts
   * are kept (kept.js). Resolves to { bitmap, png, width, height, worldPerPixel, partRect, inkRect,
   * facts, firstColor }; rejects with why it could not be drawn.
   */
  draw(blob, spec, firstColor = -1, { folderColor = -1, keep = null } = {}) {
    const drawn = this.#queue.then(() => this.#draw(blob, spec, firstColor, folderColor, keep));
    this.#queue = drawn.catch(() => {});
    return drawn;
  }

  /** Ends the worker; a later draw starts another. */
  close() {
    clearTimeout(this.#trimTimer);
    this.#worker?.terminate();
    this.#worker = null;
    this.#ready = null;
  }

  async #draw(blob, spec, firstColor, folderColor, keep) {
    clearTimeout(this.#trimTimer);
    await this.#start();
    const id = this.#next++;
    const answer = await new Promise((resolve) => {
      this.#answers.set(id, resolve);
      this.#worker.postMessage({ type: "draw", id, blob, pixels: spec.pixels, style: spec.style,
                                 colorMode: spec.colorMode, pixelsPerPoint: spec.pixelsPerPoint,
                                 lineScale: spec.lineScale, firstColor, folderColor, keep,
                                 turn: spec.turn ?? 0, worldPerPixel: spec.worldPerPixel ?? 0,
                                 supersample: spec.supersample ?? 1 });
    });
    this.heapMB = answer.heapMB ?? this.heapMB;
    if (this.heapMB > this.#recycleMB) {
      this.close();
      this.recycled++;
    } else {
      this.#trimTimer = setTimeout(() => this.#worker?.postMessage({ type: "trim" }), kTrimAfterMs);
    }
    if (!answer.bitmap) throw new Error(answer.message);
    const { bitmap, png, width, height, worldPerPixel, partRect, inkRect, facts } = answer;
    return { bitmap, png, width, height, worldPerPixel, partRect, inkRect, facts, firstColor: answer.firstColor };
  }

  #start() {
    if (this.#ready) return this.#ready;
    const worker = new Worker(this.#url, { type: "module" });
    this.#worker = worker;
    this.#ready = new Promise((resolve, reject) => {
      worker.onmessage = ({ data }) => {
        if (data.type === "ready") resolve();
        else if (data.type === "failed") reject(new Error(data.message));
        else if (data.type === "drawn") {
          const answer = this.#answers.get(data.id);
          this.#answers.delete(data.id);
          answer?.(data);
        }
      };
      worker.onerror = (event) => {
        reject(new Error(event.message ?? "The picture worker failed to start."));
        // A draw waiting on a worker that died is answered, so the queue moves on.
        for (const [id, answer] of this.#answers) answer({ id, message: "The picture worker stopped." });
        this.#answers.clear();
      };
    });
    // A worker that cannot start is not tried again for every picture.
    this.#ready.catch(() => {});
    return this.#ready;
  }
}
