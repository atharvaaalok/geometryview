// A connected folder's gallery in the page: its walk, the library drawing what is in view first, and
// the view painting the layout as facts and pictures arrive. The page's counterpart of
// App/Sources/Gallery/GalleryController.swift.

import { GalleryCore, Keys } from "./gallery-core.js";
import { GalleryLibrary, layoutFacts } from "./gallery-library.js";
import { GalleryView } from "./gallery-view.js";
import { Thumbnailer } from "./thumbs.js";

export { Actions } from "./gallery-core.js";

export class Gallery {
  core;
  view;
  root = null; // { id, name }
  #library;
  #store;
  #on;
  #entries = new Map(); // path -> entry
  #facts = new Map(); // path -> layout facts
  #pictures = new Map(); // path -> { png, note }
  #failures = new Map();
  #reloadScheduled = false;
  #wantScheduled = false;
  #walking = null;
  #style = 0;

  /**
   * `on` takes open(root, path) to open a file in the viewer, leave() when Esc reaches the top, and
   * folderChanged() for the masthead. With `probes`, texts carry baseline marks, for the checks.
   */
  constructor({ engine, store, importers, phone = false, probes = false }, on) {
    this.#store = store;
    this.#on = on;
    this.core = new GalleryCore(engine.module);
    this.view = new GalleryView(this.core, {
      action: (action) => this.#act(action),
      viewChanged: () => this.#scheduleWant(),
    }, { probes });
    this.view.element.hidden = true;
    this.#library = new GalleryLibrary({ importers, thumbnailer: new Thumbnailer(), version: engine.cacheVersion, phone }, {
      facts: (path, facts) => this.#receiveFacts(path, facts),
      picture: (path, png, note) => this.#receivePicture(path, png, note),
      failed: (path, message) => this.#receiveFailure(path, message),
    });
  }

  get title() { return this.core.title; }
  get subtitle() { return this.core.subtitle; }
  get walking() { return this.#walking !== null; }

  /** Shows `path` inside the connected folder `root` ({ id, name }), walking the folder first. */
  async show(root, path) {
    if (this.root?.id !== root.id) {
      this.root = root;
      this.#facts.clear();
      this.#pictures.clear();
      this.#failures.clear();
      this.view.clearImages();
      this.#library.setRoot(this.#store.folders, root);
      this.#load([]);
      this.#walking = this.#walk(root);
    }
    this.core.setFolder(path);
    this.view.scrollToTop();
    this.view.reload();
    this.#on.folderChanged?.();
    await this.#walking;
    this.core.setFolder(path);
    this.view.reload();
    this.#on.folderChanged?.();
    this.#scheduleWant();
  }

  async #walk(root) {
    const entries = [];
    try {
      for await (const entry of this.#store.folders.walk(root.id, "")) {
        entries.push({ path: entry.path, folder: entry.kind === "folder", bytes: entry.bytes ?? 0,
                       modified: entry.modified ?? 0 });
      }
    } catch {
      // A folder that cannot be read shows what was found.
    }
    if (this.root?.id === root.id) this.#load(entries);
    this.#walking = null;
  }

  setStyle(style) {
    this.#style = style;
    const pixels = this.core.picturePixels;
    this.#library.setSpec({ style, colorMode: 1, pixels, pixelsPerPoint: 2,
                            lineScale: Math.min(1, Math.max(0.5, pixels / 2 / 320)) });
    this.#pictures.clear();
    this.view.setStyle(style);
    this.#scheduleWant();
  }

  /** A key while the gallery shows; true if it took it. */
  key(e) {
    const keys = { ArrowLeft: Keys.Left, ArrowRight: Keys.Right, ArrowUp: Keys.Up, ArrowDown: Keys.Down,
                   Enter: Keys.Enter, " ": Keys.Space, Escape: Keys.Escape };
    let key = keys[e.key];
    if (e.key === "ArrowUp" && (e.metaKey || e.ctrlKey)) key = Keys.FolderUp;
    if (key === undefined) return false;
    this.view.key(key);
    return true;
  }

  /** Files in view still waiting for their picture, for the checks. */
  get waitingInView() {
    return this.core.wanted().filter((w) => {
      const path = this.core.path(w.file);
      return w.band <= 2 && !this.#pictures.has(path) && !this.#failures.has(path);
    }).length;
  }

  #load(entries) {
    const folder = this.core.folder;
    this.#entries = new Map(entries.map((e) => [e.path, e]));
    this.core.setEntries(this.root?.name ?? "", entries);
    this.core.setFolder(folder);
    for (const [path, facts] of this.#facts) {
      const file = this.core.fileAt(path);
      if (file !== null) this.core.setFacts(file, facts);
    }
    for (const [path, { png, note }] of this.#pictures) {
      const file = this.core.fileAt(path);
      if (file === null) continue;
      this.core.setPicture(file, note);
      this.view.setImage(file, URL.createObjectURL(png));
    }
    for (const [path, message] of this.#failures) {
      const file = this.core.fileAt(path);
      if (file !== null) this.core.setFailure(file, message);
    }
  }

  // ---- The library ----

  #scheduleWant() {
    if (this.#wantScheduled) return;
    this.#wantScheduled = true;
    queueMicrotask(() => {
      this.#wantScheduled = false;
      this.#library.want(this.core.wanted().map((w) => {
        const path = this.core.path(w.file);
        return { path, band: w.band, order: w.order, folderColor: w.folderColor, bytes: this.#entries.get(path)?.bytes ?? 0 };
      }));
    });
  }

  #receiveFacts(path, facts) {
    const file = this.core.fileAt(path);
    if (file === null) return;
    const f = layoutFacts(facts);
    this.#facts.set(path, f);
    this.core.setFacts(file, f);
    this.#scheduleReload();
  }

  #receivePicture(path, png, note) {
    const file = this.core.fileAt(path);
    if (file === null) return;
    this.#pictures.set(path, { png, note });
    this.core.setPicture(file, note);
    this.view.setImage(file, URL.createObjectURL(png));
    this.#scheduleReload(true);
  }

  #receiveFailure(path, message) {
    const file = this.core.fileAt(path);
    if (file === null) return;
    this.#failures.set(path, message);
    this.core.setFailure(file, message);
    this.#scheduleReload();
  }

  // Facts and pictures arrive in bursts: one layout a frame.
  #scheduleReload(repaint = false) {
    this.#repaint ||= repaint;
    if (this.#reloadScheduled) return;
    this.#reloadScheduled = true;
    requestAnimationFrame(() => {
      this.#reloadScheduled = false;
      if (this.#repaint) {
        this.#repaint = false;
        this.view.repaint();
      }
      this.view.reload();
    });
  }
  #repaint = false;

  #act(action) {
    switch (action.kind) {
      case 2: // into a folder
        this.#on.folderChanged?.();
        this.#scheduleWant();
        break;
      case 3: // open
      case 4: // Peek comes in G3; until then Space opens, as ⏎ does
        this.#on.open?.(this.root, this.core.path(action.target));
        break;
      case 5:
        this.#on.leave?.();
        break;
    }
  }
}
