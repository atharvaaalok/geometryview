// A connected folder's gallery in the page: its walk, the library drawing what is in view first, and
// the view painting the layout as facts and pictures arrive. The page's counterpart of
// App/Sources/Gallery/GalleryController.swift.

import { GalleryCore, Keys, Scales, Sorts } from "./gallery-core.js";
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
  #sheet = { scale: Scales.Folder, cell: 128, sort: Sorts.Name };
  #ladders = { shared: false };

  /**
   * `on` takes open(root, path) to open a file in the viewer, leave() when Esc reaches the top,
   * folderChanged() for the masthead, changed() when the view or its controls changed, for the
   * toolbar, and search(query, found) for the search box. `views` are those that may be shown, a bit
   * each. With `probes`, texts carry baseline marks, for the checks.
   */
  constructor({ engine, store, importers, phone = false, probes = false, views = 0b11 }, on) {
    this.#store = store;
    this.#on = on;
    this.core = new GalleryCore(engine.module);
    this.core.setViews(views);
    try {
      Object.assign(this.#sheet, JSON.parse(localStorage.getItem("gallery.sheet") ?? "{}"));
    } catch {
      // A browser that keeps nothing starts from the defaults.
    }
    this.core.setSheet(this.#sheet.scale, this.#sheet.cell, this.#sheet.sort);
    try {
      Object.assign(this.#ladders, JSON.parse(localStorage.getItem("gallery.ladders") ?? "{}"));
    } catch {
      // The defaults.
    }
    this.core.setLadders(this.#ladders.shared);
    this.view = new GalleryView(this.core, {
      action: (action) => this.#act(action),
      viewChanged: () => this.#scheduleWant(),
    }, { probes });
    this.view.element.hidden = true;
    this.#library = new GalleryLibrary({ importers, thumbnailer: new Thumbnailer(), version: engine.cacheVersion, phone }, {
      facts: (path, facts) => this.#receiveFacts(path, facts),
      measured: (path, bytes, modified) => this.#measured(path, bytes, modified),
      reading: (path, fraction) => {
        const file = this.core.fileAt(path);
        if (file === null) return;
        this.core.setReading(file, fraction);
        this.#scheduleReload();
      },
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
    this.#changed();
    this.#scheduleWant();
  }

  /** The view in use, those enabled, Sheet's controls and the style: what the toolbar shows. */
  get state() {
    return { view: this.core.view, views: this.core.views, sheet: { ...this.#sheet }, style: this.#style,
             ladders: { ...this.#ladders } };
  }

  /** Shows a view, if enabled: parts in both fly to their places. */
  setView(view) {
    if (!this.view.setView(view)) return false;
    this.#changed();
    this.#scheduleWant();
    return true;
  }

  /** Changes Sheet's scale, picture size or order: { scale, cell, sort }, any of them. */
  setSheet(change) {
    Object.assign(this.#sheet, change);
    try {
      localStorage.setItem("gallery.sheet", JSON.stringify(this.#sheet));
    } catch {
      // Kept for this visit only.
    }
    this.core.setSheet(this.#sheet.scale, this.#sheet.cell, this.#sheet.sort);
    this.view.reload();
    this.#changed();
    this.#scheduleWant();
  }

  /** Shows the view as a poster, dated today, or as the gallery again. */
  setPoster(poster) {
    const date = new Date().toLocaleDateString("en-GB", { day: "numeric", month: "long", year: "numeric" });
    this.core.setPoster(poster, date);
    this.view.scrollToTop();
    this.view.reload();
    this.#scheduleWant();
  }

  get poster() { return this.core.poster; }

  /** Ladders' scale: { shared }. */
  setLadders(change) {
    Object.assign(this.#ladders, change);
    try {
      localStorage.setItem("gallery.ladders", JSON.stringify(this.#ladders));
    } catch {
      // Kept for this visit only.
    }
    this.core.setLadders(this.#ladders.shared);
    this.view.reload();
    this.#changed();
    this.#scheduleWant();
  }

  /** Searches the folder in view; empty clears it. */
  setSearch(query) {
    if (query === this.core.search) return;
    this.core.setSearch(query);
    this.view.reload();
    this.#syncSearch();
  }

  #changed() {
    this.#on.changed?.();
    this.#syncSearch();
  }

  #syncSearch() {
    this.#on.search?.(this.core.search, this.core.matches);
  }

  async #walk(root) {
    const entries = [];
    let unreadable = false;
    try {
      for await (const entry of this.#store.folders.walk(root.id, "")) {
        entries.push({ path: entry.path, folder: entry.kind === "folder", bytes: entry.bytes ?? 0,
                       modified: entry.modified ?? 0 });
      }
    } catch {
      unreadable = entries.length === 0;
    }
    if (this.root?.id === root.id) {
      // A folder that cannot be read shows as last seen, faded, with Reconnect; else what was found.
      const kept = unreadable ? keptListing(root.id) : null;
      this.#setLost(Boolean(kept));
      if (kept) {
        this.#load(kept);
      } else {
        // Sizes and times already known stay with the new listing.
        const known = new Map((keptListing(root.id) ?? []).map((e) => [e.path, e]));
        for (const e of entries) {
          const k = known.get(e.path);
          if (k && !e.bytes) Object.assign(e, { bytes: k.bytes, modified: k.modified });
        }
        this.#load(entries);
        this.#scheduleKeep();
      }
    }
    this.#walking = null;
  }

  #lost = false;
  #setLost(lost) {
    this.#lost = lost;
    this.core.setLost(lost);
    this.#library.setLost(lost);
  }

  /** Permission given again: the folder is walked afresh. */
  async reconnected() {
    const root = this.root, folder = this.core.folder;
    if (!root) return;
    this.#setLost(false);
    this.root = null;
    await this.show(root, folder);
  }

  #measured(path, bytes, modified) {
    const entry = this.#entries.get(path);
    if (!entry || (entry.bytes === bytes && entry.modified === modified)) return;
    Object.assign(entry, { bytes, modified });
    this.#scheduleKeep();
  }

  // The listing, with every size and time known, kept for when the folder cannot be read.
  #keepScheduled = null;
  #scheduleKeep() {
    if (this.#lost || !this.root) return;
    clearTimeout(this.#keepScheduled);
    const id = this.root.id;
    this.#keepScheduled = setTimeout(() => keepListing(id, [...this.#entries.values()]), 2000);
  }

  setStyle(style) {
    this.#style = style;
    const pixels = this.core.picturePixels;
    this.#library.setSpec({ style, colorMode: 1, pixels, pixelsPerPoint: 2,
                            lineScale: Math.min(1, Math.max(0.5, pixels / 2 / 320)) });
    this.#pictures.clear();
    this.view.setStyle(style);
    this.#changed();
    this.#scheduleWant();
  }

  /**
   * A key while the gallery shows; true if it took it. ⌥1–⌥5 switch views (browsers keep ⌘1–⌘5);
   * typing searches, digits too, and Backspace edits the search, never going up a folder. In the
   * search box itself only Esc is the gallery's: it clears the search.
   */
  key(e) {
    if (this.core.poster) {
      // A poster takes no keys but Esc, which the page turns into putting it away.
      if (e.key === "Escape") this.#on.leavePoster?.();
      return true;
    }
    const field = e.target?.closest?.("input, textarea, select");
    if (field) {
      if (e.key !== "Escape" || field.type !== "search") return false;
      this.setSearch("");
      field.blur();
      return true;
    }
    const mod = e.metaKey || e.ctrlKey;
    if (e.altKey && !mod && e.code === "KeyP") {
      this.#on.leavePoster?.(true);
      return true;
    }
    if (e.altKey && !mod && /^Digit[1-5]$/.test(e.code)) {
      this.setView(Number(e.code.slice(5)) - 1);
      return true;
    }
    const keys = { ArrowLeft: Keys.Left, ArrowRight: Keys.Right, ArrowUp: Keys.Up, ArrowDown: Keys.Down,
                   Enter: Keys.Enter, " ": Keys.Space, Escape: Keys.Escape };
    let key = keys[e.key];
    if (e.key === "ArrowUp" && mod) key = Keys.FolderUp;
    if (key === undefined) {
      if (e.key === "Backspace" && !mod) {
        this.setSearch(Array.from(this.core.search).slice(0, -1).join(""));
        return true;
      }
      if (e.key.length === 1 && !mod && !e.altKey) {
        this.setSearch(this.core.search + e.key);
        return true;
      }
      return false;
    }
    this.view.key(key);
    this.#syncSearch();
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
        const entry = this.#entries.get(path);
        return { path, band: w.band, order: w.order, folderColor: w.folderColor, bytes: entry?.bytes ?? 0,
                 modified: entry?.modified ?? 0 };
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
      this.#syncSearch();
    });
  }
  #repaint = false;

  #act(action) {
    switch (action.kind) {
      case 2: // into a folder: it shows the view it last showed
        this.#on.folderChanged?.();
        this.#changed();
        this.#scheduleWant();
        break;
      case 3: // open
        this.#on.open?.(this.root, this.core.path(action.target));
        break;
      case 5:
        this.#on.leave?.();
        break;
      case 6: // Reconnect: asked in the click itself, as the browser wants
        this.#on.reconnect?.(this.root);
        break;
    }
  }
}

// A root's last listing, in this browser's storage: [path, folder, bytes, modified] for each entry.
// A listing past 2 MB is not kept.
function keepListing(id, entries) {
  try {
    const text = JSON.stringify(entries.map((e) => [e.path, e.folder ? 1 : 0, e.bytes ?? 0, e.modified ?? 0]));
    if (text.length < 2_000_000) localStorage.setItem(`gallery.listing.${id}`, text);
  } catch {
    // Kept for this visit only.
  }
}

function keptListing(id) {
  try {
    const rows = JSON.parse(localStorage.getItem(`gallery.listing.${id}`) ?? "null");
    return rows?.map(([path, folder, bytes, modified]) => ({ path, folder: folder === 1, bytes, modified })) ?? null;
  } catch {
    return null;
  }
}
