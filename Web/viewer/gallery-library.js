// The gallery's library in the browser: the files a gallery wants, in bands, become facts and
// pictures, from what this browser kept where it can, else by importing and drawing, as the Mac's
// library::Service does (Engine/library/service.h).
//
// A file is looked up by its key (kept.js): its facts and its picture in the current drawing spec.
// Missing either, its kept import is drawn; missing that too, it is imported first, in the gallery's
// lane of the page's importers (importers.js), so a file a person opens never waits behind it.
// Pictures are drawn in the picture worker (thumbs.js), which keeps them and the facts.

import { keptImport, keptPicture, keyOf, pictureKeyOf } from "./kept.js";

const kBands = ["open", "visible", "near", "prefetch"];

// A phone stops a page well before it would on a computer, and its gallery imports alongside its
// pictures and page: a file larger than this is not imported there. It shows as a file that could
// not be drawn, saying why, and opens on a computer. A STEP file's import takes many times its
// size (a 16 MB file 390-700 MB on an iPhone 16 Pro), an STL file's about 115 bytes a triangle.
export const kPhoneStepBytes = 12 * 1024 ** 2;
export const kPhoneStlBytes = 16 * 1024 ** 2;

function tooLargeForPhone(bytes) {
  return new Error(`Too large to draw on a phone (${Math.round(bytes / 1024 ** 2)} MB). Open it on a computer.`);
}

export class GalleryLibrary {
  #importers;
  #thumbnailer;
  #version;
  #on;
  #worker = null;
  #folders = null; // the store's folders, which read a connected folder's files
  #lost = false;

  /** The folder can no longer be read: show only what was kept. */
  setLost(lost) {
    this.#lost = lost;
    if (!lost) {
      this.#done.clear();
      this.#failed.clear();
    }
  }

  // What was kept of a file, without reading it: its key from the last listing's size and time.
  async #kept(w) {
    const root = this.#root, spec = this.#spec;
    if (!root || !spec) return;
    const key = await keyOf({ source: root.name, path: w.path, size: w.bytes, modified: w.modified,
                              version: this.#version });
    const picture = await pictureKeyOf({ ...spec, folderColor: w.folderColor, version: this.#version });
    const kept = await keptPicture(key, picture);
    if (kept?.facts) this.#on.facts?.(w.path, kept.facts);
    if (kept?.png && kept.note) this.#on.picture?.(w.path, kept.png, kept.note);
  }
  #root = null; // { id, name }
  #spec = null;
  #done = new Set(); // paths with their facts and picture in the current spec
  #failed = new Set();
  #phone;

  /**
   * `on` takes facts(path, facts), picture(path, png, note) and failed(path, message), as each is
   * known, and reading(path, fraction) while a file is imported. `version` is the cache files'
   * version, which keys hold.
   */
  // Whether the walk's sizes and times find a file in the caches, so the file itself is fetched
  // only to import it: the shell's are its own, and fetching a file there reads all of it.
  #walkStats;

  constructor({ importers, thumbnailer, version, phone = false, walkStats = false }, on) {
    this.#walkStats = walkStats;
    this.#importers = importers;
    this.#thumbnailer = thumbnailer;
    this.#version = version;
    this.#phone = phone;
    this.#on = on;
  }

  /** The connected folder the gallery shows, and the store's folders that read it. */
  setRoot(folders, root) {
    this.#folders = folders;
    this.#root = root;
    this.#done.clear();
    this.#failed.clear();
  }

  /** How pictures are drawn: { style, colorMode, pixels, pixelsPerPoint, lineScale }. */
  setSpec(spec) {
    this.#spec = spec;
    this.#done.clear();
  }

  /**
   * Replaces what is wanted: [{ path, band (1 in view .. 3 elsewhere), order, folderColor, bytes,
   * modified }]. While the folder cannot be read, only what was kept is shown, found by the size and
   * time the last listing gave.
   */
  want(wants) {
    const jobs = [];
    for (const w of wants) {
      if (this.#done.has(w.path) || this.#failed.has(w.path)) continue;
      jobs.push({ id: w.path, band: kBands[w.band] ?? "prefetch", order: w.order, bytes: w.bytes ?? 0,
                  run: (signal) => (this.#lost ? this.#kept(w) : this.#draw(w.path, w.folderColor, signal, w)) });
    }
    this.#importers.want(jobs, {
      onDone: (path) => this.#done.add(path),
      onFailed: (path, error) => {
        this.#failed.add(path);
        this.#on.failed?.(path, String(error?.message ?? error));
      },
    });
  }

  // Where a file is in the caches: its key, from the size and time its walk gave where they are
  // trusted, else from the file itself, fetched. { key, bytes, file }, the file null until needed.
  async #locate(path, stat) {
    const root = this.#root;
    let file = null, bytes = stat?.bytes ?? 0, modified = stat?.modified ?? 0;
    if (!this.#walkStats || !(bytes > 0 && modified > 0)) {
      file = await this.#folders.file(root.id, path);
      if (!file) throw new Error("This file could not be read.");
      bytes = file.size;
      modified = file.lastModified;
      // Its size and time, kept with the listing, find its picture again if the folder cannot be read.
      this.#on.measured?.(path, bytes, modified);
    }
    const key = await keyOf({ source: root.name, path, size: bytes, modified, version: this.#version });
    return { key, bytes, file };
  }

  // The file itself, to import it: fetched now if its walk found it.
  async #fileOf(path, located) {
    located.file ??= await this.#folders.file(this.#root.id, path);
    if (!located.file) throw new Error("This file could not be read.");
    return located.file;
  }

  #tooLarge(path, bytes) {
    return this.#phone && bytes > (/\.stl$/i.test(path) ? kPhoneStlBytes : kPhoneStepBytes);
  }

  async #draw(path, folderColor, signal, stat) {
    const root = this.#root, spec = this.#spec;
    if (!root || !spec) return;
    const located = await this.#locate(path, stat);
    const { key } = located;
    const picture = await pictureKeyOf({ ...spec, folderColor, version: this.#version });
    // Kept before: its facts at once, and its picture if drawn in this spec.
    const kept = await keptPicture(key, picture);
    if (kept?.facts) this.#on.facts?.(path, kept.facts);
    if (kept?.png && kept.note) {
      this.#on.picture?.(path, kept.png, kept.note);
      return;
    }
    if (signal.aborted) throw new Error("cancelled");
    // Its import kept before, or imported now.
    let blob = (await keptImport(key, this.#version))?.blob;
    if (!blob && this.#tooLarge(path, located.bytes)) throw tooLargeForPhone(located.bytes);
    if (!blob) {
      this.#on.reading?.(path, 0);
      const file = await this.#fileOf(path, located);
      blob = await this.#import(file, key, signal, (fraction) => this.#on.reading?.(path, fraction));
    }
    const drawn = await this.#thumbnailer.draw(blob, spec, -1, { folderColor, keep: { key, picture } });
    drawn.bitmap?.close();
    if (drawn.facts) this.#on.facts?.(path, drawn.facts);
    this.#on.picture?.(path, drawn.png, { width: drawn.width, height: drawn.height, mmPerPixel: drawn.worldPerPixel,
                                          part: drawn.partRect, ink: drawn.inkRect, firstColor: drawn.firstColor });
  }

  /**
   * A file's detail picture, for where it is drawn large: the same picture at twice the pixels and
   * twice the pixels a point, kept like the gallery's and read back if kept. The file is imported
   * first if its import was not kept. Resolves to { png, note }, or null when it cannot be drawn.
   */
  async detail(path, folderColor, signal, stat) {
    const root = this.#root, spec = this.#spec;
    if (!root || !spec) return null;
    const detail = { ...spec, pixels: spec.pixels * 2, pixelsPerPoint: spec.pixelsPerPoint * 2, supersample: 1 };
    const located = await this.#locate(path, stat).catch(() => null);
    if (!located) return null;
    const { key } = located;
    const picture = await pictureKeyOf({ ...detail, folderColor, version: this.#version });
    const kept = await keptPicture(key, picture);
    if (kept?.png && kept.note) return { png: kept.png, note: kept.note };
    if (signal.aborted) return null;
    let blob = (await keptImport(key, this.#version))?.blob;
    if (!blob && this.#tooLarge(path, located.bytes)) return null;
    if (!blob) blob = await this.#import(await this.#fileOf(path, located), key, signal);
    if (signal.aborted) return null;
    const drawn = await this.#thumbnailer.draw(blob, detail, -1, { folderColor, keep: { key, picture } });
    drawn.bitmap?.close();
    return { png: drawn.png, note: { width: drawn.width, height: drawn.height, mmPerPixel: drawn.worldPerPixel,
                                     part: drawn.partRect, ink: drawn.inkRect } };
  }

  /**
   * Peek's turntable: `frames` pictures of the file at `path`, `pixels` square, turned evenly about
   * the upright at one scale (its box's diagonal, from `size` in millimetres, fits whichever way it
   * faces), each handed to `onFrame(frame, bitmap, note)` as it is drawn. The file is imported first
   * if its import was not kept. Stops when `signal` aborts.
   */
  async turn(path, folderColor, frames, pixels, size, onFrame, signal, stat) {
    const root = this.#root, spec = this.#spec;
    if (!root || !spec) return false;
    const located = await this.#locate(path, stat).catch(() => null);
    if (!located) return false;
    const { key } = located;
    let blob = (await keptImport(key, this.#version))?.blob;
    if (!blob && this.#tooLarge(path, located.bytes)) return false; // too large here
    if (!blob) {
      if (signal.aborted) return false;
      blob = await this.#import(await this.#fileOf(path, located), key, signal);
    }
    const worldPerPixel = size ? Math.hypot(...size) * 1.1 / pixels : 0;
    for (let frame = 0; frame < frames; frame++) {
      if (signal.aborted) return false;
      // Drawn as a detail is: lines as thick for its size as a picture's, and not supersampled.
      const drawn = await this.#thumbnailer.draw(blob, { ...spec, pixels, turn: (360 * frame) / frames, worldPerPixel,
                                                         pixelsPerPoint: spec.pixelsPerPoint * pixels / spec.pixels,
                                                         supersample: 1 }, -1, { folderColor });
      onFrame(frame, drawn.bitmap, { width: drawn.width, height: drawn.height, ink: drawn.inkRect });
    }
    return !signal.aborted;
  }

  // Imports in the gallery's own worker; a file in a connected folder keeps no source copy.
  async #import(file, key, signal, progress) {
    this.#worker ??= new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" });
    const worker = this.#worker;
    const stl = /\.stl$/i.test(file.name);
    const result = await new Promise((resolve) => {
      worker.onmessage = ({ data }) => {
        if (data.type === "progress" && data.total > 0) progress?.(data.done / data.total);
        if (data.type === "done" || data.type === "failed") resolve(data);
      };
      worker.onerror = (event) => resolve({ type: "failed", message: event.message ?? "The importer failed." });
      signal.addEventListener("abort", () => worker.postMessage({ type: "cancel" }), { once: true });
      worker.postMessage({ type: "import", file, kind: stl ? "stl" : "step", name: file.name, key, keepSource: false,
                           workers: this.#phone ? 1 : 2, threads: false, progressive: false });
    });
    // A phone lets its importer go after each file: its heap only grows, and the page's memory is
    // what the phone watches.
    if (this.#phone) {
      worker.terminate();
      this.#worker = null;
    }
    if (result.type !== "done") throw new Error(result.message ?? "This file could not be read.");
    const kept = await keptImport(key, this.#version);
    if (!kept) throw new Error("The import was not kept.");
    return kept.blob;
  }
}

/** Facts as kept (scene::Facts' JSON) to the layout's: sides in millimetres, parts and name. */
export function layoutFacts(facts) {
  const hasBox = Array.isArray(facts.min) && Array.isArray(facts.max);
  return {
    size: hasBox ? facts.max.map((v, i) => v - facts.min[i]) : [0, 0, 0],
    hasBox, estimated: Boolean(facts.estimated), parts: facts.parts ?? 0, product: facts.name ?? "",
    triangles: facts.triangles ?? 0,
  };
}
