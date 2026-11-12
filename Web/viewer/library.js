// The library: connected folders and recent files, in the Library card; and the imports this
// browser keeps in its origin-private file system as cache files, so that a file opens again at
// once without importing. What a folder holds and where files come from is library-store.js's;
// which rows show, library-rows.js's.

import { iconNode, icons } from "./icons.js";
import { ageText, RowKinds, rowsOf, keyOf, Sections } from "./library-rows.js";
import { el, pill, segmented, VirtualList } from "./ui.js";

const kFolder = "library";

async function folder() {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(kFolder, { create: true });
}

// Every kept file: { name, size, modified, file() }.
export async function libraryEntries() {
  const entries = [];
  try {
    for await (const handle of (await folder()).values()) {
      if (handle.kind !== "file" || !handle.name.endsWith(".geoblob")) continue;
      const file = await handle.getFile();
      entries.push({ name: handle.name.replace(/\.geoblob$/, ""), size: file.size,
                     modified: file.lastModified, file: () => handle.getFile() });
    }
  } catch {
    // No origin-private file system here: nothing is kept.
  }
  return entries.sort((a, b) => b.modified - a.modified);
}

// The cache file version the engine reads, set once it has started: a kept import written in
// another is as good as none, and is made again.
let cacheVersion = null;
export function setCacheVersion(version) { cacheVersion = version; }

// A cache file's version, from its header: 8 bytes of magic, then a little-endian u32.
async function versionOf(blob) {
  const header = new DataView(await blob.slice(0, 12).arrayBuffer());
  return header.byteLength >= 12 ? header.getUint32(8, true) : -1;
}

// A kept cache file, by the name of the file it came from, or null if there is none or it is in
// an older format.
async function current(dir, name) {
  const blob = await (await dir.getFileHandle(`${name}.geoblob`)).getFile();
  return cacheVersion === null || (await versionOf(blob)) === cacheVersion ? blob : null;
}

// The cache file an import of `file` made before, if the file is unchanged since: its size and
// modification time match the note kept with it, and the engine reads its version.
export async function cachedImport(file) {
  try {
    const dir = await folder();
    const note = JSON.parse(await (await (await dir.getFileHandle(`${file.name}.json`)).getFile()).text());
    if (note.size !== file.size || note.lastModified !== file.lastModified) return null;
    return await current(dir, file.name);
  } catch {
    return null;
  }
}

// A kept cache file, by the name of the file it came from; null if it is in an older format.
export async function keptFile(name) {
  return current(await folder(), name);
}

// Deletes what was kept for `name`: its cache file, its note and its source.
export async function forgetKept(name) {
  try {
    const dir = await folder();
    for (const suffix of [".geoblob", ".json", ".source"]) {
      await dir.removeEntry(`${name}${suffix}`).catch(() => {});
    }
  } catch {
    // Nothing kept here.
  }
}

// A STEP file kept beside its cache, for importing its definitions again, finer, or null if none
// was kept whole.
export async function keptSource(name) {
  try {
    const dir = await folder();
    const note = JSON.parse(await (await (await dir.getFileHandle(`${name}.json`)).getFile()).text());
    const source = await (await dir.getFileHandle(`${name}.source`)).getFile();
    return source.size === note.size ? source : null;
  } catch {
    return null;
  }
}

// A row's height, as the structure tree's.
const rowHeight = () => (document.body.classList.contains("phone") ? 30 : 28);

// The Library card: its switcher and the two ways in, opening files and connecting a folder, then
// Folders and Recent as rows like the structure tree's.
export class LibraryCard {
  #store;
  #on;
  #list;
  #rows = [];
  #folders = [];
  #listings = new Map();
  #expanded = new Set();
  #collapsed;
  #recent = [];
  #open = new Set();

  // `on` takes: open({ root, path, name }, add), reopen(entry, add), choose(), connect(), switch(show),
  // showFolder(root, path)
  constructor(store, on) {
    this.#store = store;
    this.#on = on;
    try {
      this.#collapsed = new Set(JSON.parse(localStorage.getItem("library.collapsed") ?? "[]"));
    } catch {
      this.#collapsed = new Set();
    }
    this.switcher = segmented(["Library", "Structure"], { selected: 0, help: "Library or structure",
                                                          onChange: (i) => on.switch(i === 0) });
    const add = pill({ icon: icons.plus, help: "Open files (⌘O)", onPress: () => on.choose() });
    const connect = pill({ icon: icons.folderPlus, help: "Connect a folder (⌥⌘O)", onPress: () => on.connect() });
    connect.hidden = !store.canConnect;
    this.#list = new VirtualList({ role: "tree", rowHeight, count: () => this.#rows.length,
                                   row: (index) => this.#row(this.#rows[index]) });
    this.element = el("section.card.structure.library", { hidden: true, "aria-label": "Library" }, [
      el("div.header", {}, [this.switcher.element, el("span.grow"), add, connect]),
      this.#list.element,
    ]);
  }

  // Reads the folders and recent files again; `kept` are the imports this browser kept, which
  // fill Recent on a first visit.
  async refresh(kept) {
    this.#folders = await this.#store.folders.folders();
    this.#recent = await this.#store.recent(kept);
    this.#rebuild();
  }

  // The keys of the files shown now, marked in the rows.
  setOpen(keys) {
    this.#open = new Set(keys);
    this.#rebuild();
  }

  // Opens a newly connected folder's row.
  expand(root) {
    const key = keyOf(root, "");
    this.#expanded.add(key);
    this.#readListing(root, "");
  }

  folderName(id) {
    return this.#folders.find((f) => f.id === id)?.name ?? "";
  }

  get readyFolders() {
    return new Set(this.#folders.filter((f) => f.state === "ready" || f.state === "visit").map((f) => f.id));
  }

  #rebuild() {
    this.#rows = rowsOf({ folders: this.#folders, listings: this.#listings, expanded: this.#expanded,
                          collapsed: this.#collapsed, recent: this.#recent, open: this.#open });
    if (!this.element.hidden) this.#list.render();
  }

  render() { this.#rebuild(); this.#list.render(); }

  // A folder is listed each time it is opened, so opening it again shows what changed; "Reading…"
  // only if that takes a moment.
  async #readListing(root, path) {
    const key = keyOf(root, path);
    const listing = { state: "reading", slow: false };
    this.#listings.set(key, listing);
    const slow = setTimeout(() => { listing.slow = true; this.#rebuild(); }, 150);
    this.#rebuild();
    try {
      const entries = await this.#store.folders.list(root, path);
      this.#listings.set(key, { state: "listed", entries });
    } catch {
      this.#listings.set(key, { state: "failed" });
    }
    clearTimeout(slow);
    this.#rebuild();
  }

  #toggleSection(key) {
    if (this.#collapsed.has(key)) this.#collapsed.delete(key);
    else this.#collapsed.add(key);
    try {
      localStorage.setItem("library.collapsed", JSON.stringify([...this.#collapsed]));
    } catch {
      // Not remembered: it costs nothing now.
    }
    this.#rebuild();
  }

  async #toggleFolder(row) {
    if (row.state === "reconnect") {
      if (await this.#store.folders.reconnect(row.root)) {
        await this.refresh();
        this.expand(row.root);
      }
      return;
    }
    if (row.state === "missing") return;
    if (this.#expanded.has(row.key)) {
      this.#expanded.delete(row.key);
      this.#rebuild();
    } else {
      this.#expanded.add(row.key);
      await this.#readListing(row.root, row.path);
    }
    // A folder's gallery shows beside the card.
    this.#on.showFolder?.({ id: row.root, name: this.folderName(row.root) }, row.path);
  }

  async #disconnect(row) {
    await this.#store.folders.disconnect(row.root);
    for (const key of [...this.#expanded]) if (key.startsWith(`${row.root}/`)) this.#expanded.delete(key);
    for (const key of [...this.#listings.keys()]) if (key.startsWith(`${row.root}/`)) this.#listings.delete(key);
    await this.refresh();
  }

  async #forget(row) {
    await this.#on.forget(row.recentKey);
    await this.refresh();
  }

  #row(row) {
    const element = el("div.row", { role: "treeitem" });
    element.style.paddingLeft = `${8 + row.depth * 14}px`;
    if (row.kind === RowKinds.Message) {
      element.classList.add("message");
      element.style.paddingLeft = `${8 + row.depth * 14 + 35}px`; // under the names at its depth
      element.append(el("span.name", { text: row.text }));
      return element;
    }
    const expandable = row.kind === RowKinds.Section ||
                       (row.kind === RowKinds.Folder && row.state !== "missing");
    if (expandable) {
      const chevron = el("button.chevron", { type: "button", "aria-label": row.expanded ? "Collapse" : "Expand" },
                         [iconNode("chevron", 12)]);
      chevron.classList.toggle("open", row.expanded);
      element.append(chevron);
    } else {
      element.append(el("span.chevron"));
    }
    const actions = [];
    let end = null;
    switch (row.kind) {
      case RowKinds.Section:
        element.classList.add("heading");
        element.append(el("span.name", { text: row.name }));
        end = el("span.count", { text: String(row.count) });
        element.addEventListener("click", () => this.#toggleSection(row.key));
        break;
      case RowKinds.Folder:
        element.append(el("span.glyph", {}, [iconNode("folder", 14)]), el("span.name", { text: row.name }));
        element.classList.toggle("missing", row.state === "missing");
        if (row.top) {
          element.title = row.name;
          actions.push(this.#rowButton("Disconnect", "close", () => this.#disconnect(row)));
          if (row.state === "reconnect") {
            end = pill({ title: "Reconnect", help: "Let this page read the folder again",
                         onPress: () => this.#toggleFolder(row) });
            end.classList.add("small");
          } else if (row.state === "missing") {
            end = el("span.count", { text: "Missing" });
          } else if (row.state === "visit") {
            end = el("span.count", { text: "this visit", title: "This browser keeps the folder only while the page is open" });
          }
        }
        element.addEventListener("click", () => this.#toggleFolder(row));
        break;
      case RowKinds.File:
        element.append(el("span.glyph", {}, [iconNode("cube", 14)]), el("span.name", { text: row.name }));
        element.classList.toggle("selected", Boolean(row.open));
        element.title = "Open; ⌘-click adds it to what is shown";
        element.addEventListener("click", (e) => this.#on.open(row, e.metaKey || e.ctrlKey));
        break;
      case RowKinds.Recent:
        element.append(el("span.glyph", {}, [iconNode("cube", 14)]), el("span.name", { text: row.name }));
        element.classList.toggle("selected", Boolean(row.open));
        element.title = row.where ?? row.name;
        actions.push(this.#rowButton("Remove from Recent", "close", () => this.#forget(row)));
        end = el("span.count", { text: ageText(row.openedAt) });
        element.addEventListener("click", (e) => {
          const entry = this.#recent.find((r) => r.key === row.recentKey);
          if (entry) this.#on.reopen(entry, e.metaKey || e.ctrlKey);
        });
        break;
    }
    if (actions.length) element.append(el("span.actions", {}, actions));
    if (end) element.append(end);
    return element;
  }

  #rowButton(help, icon, press) {
    const button = el("button.rowbutton", { type: "button", title: help, "aria-label": help }, [iconNode(icon, 13)]);
    button.addEventListener("click", (e) => {
      e.stopPropagation();
      press();
    });
    return button;
  }
}
