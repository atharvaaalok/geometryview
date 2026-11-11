// Where the Library's folders and recent files come from, and how a file in one is read: one
// interface over the three ways a page can reach folders.
//
//   shell   The iPhone shell picks folders and files natively, keeps them, and serves them.
//   access  The File System Access API (Chrome, Edge): folders are handles, kept in IndexedDB from
//           one visit to the next; the browser may ask again for permission on a later visit.
//   visit   Elsewhere (Safari, Firefox), a folder chosen with a folder input or dropped lasts
//           for this visit only.
//
// Nothing is read until it is shown: a folder is listed when it is opened, by names alone, and a
// file is read when it is opened. A gallery walks a folder with walk(), which yields its folders
// and STEP and STL files, by names first wherever the browser allows. The recent files are kept in
// IndexedDB everywhere.

import { keptImport } from "./kept.js";
import { keptFile } from "./library.js";
import { remember as rememberIn } from "./library-rows.js";

const kDatabase = "geometryview";

function database() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(kDatabase, 1);
    request.onupgradeneeded = () => {
      request.result.createObjectStore("folders", { keyPath: "id" });
      request.result.createObjectStore("recent", { keyPath: "key" });
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

let opened = null;
async function store(name, mode, work) {
  opened ??= database();
  const db = await opened;
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(name, mode);
    const result = work(transaction.objectStore(name));
    transaction.oncomplete = () => resolve(result?.result);
    transaction.onerror = () => reject(transaction.error);
  });
}
const all = (name) => store(name, "readonly", (s) => s.getAll()).catch(() => []);
const put = (name, value) => store(name, "readwrite", (s) => s.put(value)).catch(() => {});
const remove = (name, key) => store(name, "readwrite", (s) => s.delete(key)).catch(() => {});

// What a gallery shows, and how far a walk goes, as on the Mac (Engine/library/scan.h).
export const isModelName = (name) => /\.(step|stp|stl)$/i.test(name);
export const kWalkLimits = { depth: 32, files: 20000 };

const kModelTypes = [{ description: "STEP, STL or cache files",
                       accept: { "application/octet-stream": [".step", ".stp", ".stl", ".geoblob"] } }];

// ---- File System Access -----------------------------------------------------------------------

class AccessFolders {
  kind = "access";
  canConnect = true;
  #folders = null; // [{ id, name, handle }]

  async #all() {
    this.#folders ??= await all("folders");
    return this.#folders;
  }

  async folders() {
    const folders = await this.#all();
    return Promise.all(folders.map(async ({ id, name, handle }) => {
      const permission = await handle.queryPermission({ mode: "read" }).catch(() => "denied");
      return { id, name, state: permission === "granted" ? "ready" : "reconnect" };
    }));
  }

  // Asks for a folder; null if the person turns the picker away.
  async connect() {
    let handle;
    try {
      handle = await showDirectoryPicker({ id: "geometryview-library", mode: "read" });
    } catch (error) {
      if (error.name === "AbortError") return null;
      throw error;
    }
    return this.adoptHandle(handle);
  }

  async adoptHandle(handle) {
    const folders = await this.#all();
    for (const folder of folders) {
      if (await folder.handle.isSameEntry(handle).catch(() => false)) return folder.id;
    }
    const folder = { id: crypto.randomUUID(), name: handle.name, handle };
    folders.push(folder);
    await put("folders", folder);
    return folder.id;
  }

  // The tap is the gesture the browser needs to ask again.
  async reconnect(id) {
    const folder = (await this.#all()).find((f) => f.id === id);
    return (await folder?.handle.requestPermission({ mode: "read" })) === "granted";
  }

  async disconnect(id) {
    this.#folders = (await this.#all()).filter((f) => f.id !== id);
    await remove("folders", id);
  }

  async #directory(id, path) {
    let directory = (await this.#all()).find((f) => f.id === id)?.handle;
    if (!directory) throw new Error("That folder is no longer connected.");
    for (const part of path.split("/").filter(Boolean)) directory = await directory.getDirectoryHandle(part);
    return directory;
  }

  async list(id, path) {
    const entries = [];
    for await (const [name, handle] of (await this.#directory(id, path)).entries()) {
      entries.push({ name, kind: handle.kind === "directory" ? "folder" : "file" });
    }
    return entries;
  }

  async fileHandle(id, path) {
    const parts = path.split("/");
    const name = parts.pop();
    return (await this.#directory(id, parts.join("/"))).getFileHandle(name);
  }

  // A gallery's walk, by names alone: { path, kind, handle } for each folder and STEP or STL file.
  // A file's size and time come from handle.getFile(), asked for when its tile is in view.
  async *walk(id, path = "", { depth = kWalkLimits.depth, files = kWalkLimits.files } = {}) {
    const start = await this.#directory(id, path);
    const pending = [[start, path, 0]];
    let found = 0;
    while (pending.length > 0) {
      const [directory, at, level] = pending.shift();
      for await (const [name, handle] of directory.entries()) {
        if (name.startsWith(".")) continue;
        const inner = at ? `${at}/${name}` : name;
        if (handle.kind === "directory") {
          yield { path: inner, kind: "folder", handle };
          if (level + 1 < depth) pending.push([handle, inner, level + 1]);
        } else if (isModelName(name)) {
          if (found++ === files) return;
          yield { path: inner, kind: "file", handle };
        }
      }
    }
  }

  async file(id, path) {
    return (await this.fileHandle(id, path)).getFile();
  }

  // Files to open, each with its handle so Recent can read it again; null if turned away.
  async pickFiles() {
    try {
      const handles = await showOpenFilePicker({ id: "geometryview-open", multiple: true, types: kModelTypes });
      return Promise.all(handles.map(async (handle) => ({ file: await handle.getFile(), handle })));
    } catch (error) {
      if (error.name === "AbortError") return null;
      throw error;
    }
  }
}

// ---- A folder for this visit ------------------------------------------------------------------

class VisitFolders {
  kind = "visit";
  canConnect = "webkitdirectory" in HTMLInputElement.prototype;
  #folders = []; // [{ id, name, files?: Map(path, File), entry?: DirectoryEntry }]

  async folders() {
    return this.#folders.map(({ id, name }) => ({ id, name, state: "visit" }));
  }

  // A folder input: every file in the folder arrives at once, with its path inside it.
  connect() {
    return new Promise((resolve) => {
      const input = Object.assign(document.createElement("input"), { type: "file", webkitdirectory: true });
      input.addEventListener("change", () => {
        const files = [...input.files];
        if (files.length === 0) return resolve(null);
        const name = files[0].webkitRelativePath.split("/")[0];
        const byPath = new Map(files.map((f) => [f.webkitRelativePath.split("/").slice(1).join("/"), f]));
        resolve(this.adoptFiles(name, byPath));
      });
      input.addEventListener("cancel", () => resolve(null));
      input.click();
    });
  }

  adoptEntry(entry) {
    return this.#add({ name: entry.name, entry });
  }

  // A folder as a Map of the paths inside it to its files.
  adoptFiles(name, files) {
    return this.#add({ name, files });
  }

  #add(folder) {
    const id = crypto.randomUUID();
    this.#folders.push({ id, ...folder });
    return id;
  }

  async reconnect() { return true; }

  async disconnect(id) {
    this.#folders = this.#folders.filter((f) => f.id !== id);
  }

  async list(id, path) {
    const folder = this.#folders.find((f) => f.id === id);
    if (!folder) throw new Error("That folder is no longer connected.");
    if (folder.files) {
      const prefix = path ? `${path}/` : "";
      const seen = new Map();
      for (const inner of folder.files.keys()) {
        if (!inner.startsWith(prefix)) continue;
        const [name, ...rest] = inner.slice(prefix.length).split("/");
        if (!seen.has(name)) seen.set(name, rest.length > 0 ? "folder" : "file");
      }
      return [...seen].map(([name, kind]) => ({ name, kind }));
    }
    const directory = await this.#entry(folder.entry, path);
    const entries = [];
    const reader = directory.createReader();
    for (;;) {
      const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
      if (batch.length === 0) break;
      for (const e of batch) entries.push({ name: e.name, kind: e.isDirectory ? "folder" : "file" });
    }
    return entries;
  }

  async #entry(directory, path) {
    for (const part of path.split("/").filter(Boolean)) {
      directory = await new Promise((resolve, reject) => directory.getDirectory(part, {}, resolve, reject));
    }
    return directory;
  }

  async file(id, path) {
    const folder = this.#folders.find((f) => f.id === id);
    if (!folder) throw new Error("That folder is no longer connected.");
    if (folder.files) return folder.files.get(path);
    const parts = path.split("/");
    const name = parts.pop();
    const directory = await this.#entry(folder.entry, parts.join("/"));
    const entry = await new Promise((resolve, reject) => directory.getFile(name, {}, resolve, reject));
    return new Promise((resolve, reject) => entry.file(resolve, reject));
  }

  pickFiles() { return Promise.resolve(undefined); } // the page's own file input instead

  // A gallery's walk: { path, kind, file? }, files with their File where the folder came whole.
  async *walk(id, path = "", { depth = kWalkLimits.depth, files = kWalkLimits.files } = {}) {
    const folder = this.#folders.find((f) => f.id === id);
    if (!folder) throw new Error("That folder is no longer connected.");
    let found = 0;
    if (folder.files) {
      const prefix = path ? `${path}/` : "";
      const folders = new Set();
      for (const [inner, file] of folder.files) {
        if (!inner.startsWith(prefix)) continue;
        const parts = inner.slice(prefix.length).split("/");
        if (parts.some((part) => part.startsWith(".")) || parts.length > depth) continue;
        for (let i = 1; i < parts.length; i++) {
          const dir = prefix + parts.slice(0, i).join("/");
          if (!folders.has(dir)) {
            folders.add(dir);
            yield { path: dir, kind: "folder" };
          }
        }
        if (!isModelName(inner)) continue;
        if (found++ === files) return;
        yield { path: inner, kind: "file", file };
      }
      return;
    }
    const pending = [[path, 0]];
    while (pending.length > 0) {
      const [at, level] = pending.shift();
      for (const entry of await this.list(id, at)) {
        if (entry.name.startsWith(".")) continue;
        const inner = at ? `${at}/${entry.name}` : entry.name;
        if (entry.kind === "folder") {
          yield { path: inner, kind: "folder" };
          if (level + 1 < depth) pending.push([inner, level + 1]);
        } else if (isModelName(entry.name)) {
          if (found++ === files) return;
          yield { path: inner, kind: "file" };
        }
      }
    }
  }
}

// ---- The iPhone shell ------------------------------------------------------------------------

const encodePath = (path) => path.split("/").map(encodeURIComponent).join("/");

class ShellFolders {
  kind = "shell";
  canConnect = true;

  constructor() {
    // The shell serves the page from the same address each launch, so what it keeps lasts: asked
    // not to be cleared under storage pressure.
    navigator.storage?.persist?.().catch(() => {});
  }

  #ask(op, extra = {}) {
    return window.webkit.messageHandlers.library.postMessage({ op, ...extra });
  }

  folders() { return this.#ask("folders"); }
  async connect() { return (await this.#ask("connectFolder")) ?? null; }
  async reconnect() { return true; }
  async disconnect(id) { await this.#ask("disconnect", { id }); }

  async list(id, path) {
    const response = await fetch(`/list/${id}/${encodePath(path)}`);
    if (!response.ok) throw new Error("This folder could not be read.");
    return response.json();
  }

  async file(id, path) {
    return this.#fetchFile(`/open/${id}/${encodePath(path)}`, path.split("/").pop());
  }

  // A gallery's walk, from the shell's /scan, which walks the folder natively: { path, kind, bytes,
  // modified, cloud }, a file that lives only in iCloud marked cloud.
  async *walk(id, path = "", { depth = kWalkLimits.depth, files = kWalkLimits.files } = {}) {
    const response = await fetch(`/scan/${id}/${encodePath(path)}?depth=${depth}&limit=${files}`);
    if (!response.ok) throw new Error("This folder could not be read.");
    const prefix = path ? `${path}/` : "";
    for (const line of (await response.text()).split("\n")) {
      if (!line) continue;
      const entry = JSON.parse(line);
      if (entry.done) return;
      yield { ...entry, path: prefix + entry.path };
    }
  }

  async #fetchFile(url, name) {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`${name} could not be read.`);
    const lastModified = Number(response.headers.get("GeometryView-Modified")) || Date.now();
    return new File([await response.blob()], name, { lastModified });
  }

  // The system's picker, opening files where they are.
  async pickFiles() {
    const picked = await this.#ask("pickFiles");
    if (!picked) return null;
    return Promise.all(picked.map(async (p) => ({ file: await this.#fetchFile(p.path, p.name), handle: null })));
  }
}

// ---- The store --------------------------------------------------------------------------------

export class LibraryStore {
  folders;
  #recent = null;

  // `backend` "visit" uses a folder for this visit even where the browser offers more, for checks.
  constructor({ backend } = {}) {
    const shell = window.webkit?.messageHandlers?.library !== undefined;
    const access = typeof showDirectoryPicker === "function" && backend !== "visit";
    this.folders = shell ? new ShellFolders() : access ? new AccessFolders() : new VisitFolders();
  }

  get kind() { return this.folders.kind; }
  get canConnect() { return this.folders.canConnect; }

  // Dropped items, read in the drop event itself, as browsers require: folders are connected and
  // files returned to open, each with its handle where the browser gives one. Returns
  // { files: [{ file, handle }], connected: [ids] }.
  async adopt(dataTransfer) {
    const items = [...dataTransfer.items].filter((item) => item.kind === "file");
    // Every handle asked for before any await, while the drop's data can still be read.
    const handles = items.map((item) => item.getAsFileSystemHandle?.() ?? null);
    const entries = items.map((item) => item.webkitGetAsEntry?.() ?? null);
    const plain = items.map((item) => item.getAsFile());
    const files = [];
    const connected = [];
    for (let i = 0; i < items.length; ++i) {
      const handle = await handles[i]?.catch(() => null);
      if (handle?.kind === "directory" && this.folders.adoptHandle) {
        connected.push(await this.folders.adoptHandle(handle));
      } else if (entries[i]?.isDirectory && this.folders.adoptEntry) {
        connected.push(this.folders.adoptEntry(entries[i]));
      } else if (plain[i]) {
        files.push({ file: plain[i], handle: handle?.kind === "file" ? handle : null });
      }
    }
    return { files, connected };
  }

  // ---- Recent files ----

  // The recent files, newest first: { key, name, where, openedAt, handle?, root?, path? }. On a
  // first visit, the imports this browser kept before Recent existed.
  async recent(kept) {
    if (this.#recent) return this.#recent;
    const saved = await all("recent");
    if (saved.length === 0 && kept?.length) {
      this.#recent = kept.slice(0, 12).map((k) => ({ key: k.name, name: k.name, where: "Kept in this browser",
                                                     openedAt: k.modified }));
      for (const r of this.#recent) await put("recent", r);
    } else {
      this.#recent = saved.sort((a, b) => b.openedAt - a.openedAt);
    }
    return this.#recent;
  }

  // Puts `entry` first; returns the entries that fell off.
  async remember(entry) {
    const { recent, dropped } = rememberIn(await this.recent(), entry);
    this.#recent = recent;
    await put("recent", entry);
    for (const old of dropped) await remove("recent", old.key);
    return dropped;
  }

  async forget(key) {
    this.#recent = (await this.recent()).filter((r) => r.key !== key);
    await remove("recent", key);
  }

  // What reopening a recent file gives: { file } to import (its kept import is used if it is
  // unchanged), { cache } to open as it was kept, or null if it can't be had. May ask for
  // permission, so call it from the click.
  async reopen(entry, readyFolders) {
    if (entry.root && readyFolders.has(entry.root)) {
      return { file: await this.folders.file(entry.root, entry.path) };
    }
    const permission = await entry.handle?.queryPermission?.({ mode: "read" }).catch(() => "denied");
    if (permission === "granted") return { file: await entry.handle.getFile() };
    const cache = (entry.cacheKey ? (await keptImport(entry.cacheKey))?.blob : null) ??
                  await keptFile(entry.name).catch(() => null);
    if (cache) return { cache };
    if (entry.handle && (await entry.handle.requestPermission({ mode: "read" }).catch(() => "denied")) === "granted") {
      return { file: await entry.handle.getFile() };
    }
    if (entry.url) return { url: entry.url };
    return null;
  }
}
