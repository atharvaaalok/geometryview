// The library: files this browser has opened, kept in its origin-private file system as cache
// files, so they open again at once without importing. Listed newest first.

import { el, pill, segmented } from "./ui.js";

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

// A writable stream for a cache file kept under `name`, replacing one kept before.
export async function keepInLibrary(name) {
  const handle = await (await folder()).getFileHandle(`${name}.geoblob`, { create: true });
  return handle.createWritable();
}

export class LibraryCard {
  #list = el("div.entries");
  #onOpen;

  constructor({ onOpen, onChoose, onSwitch }) {
    this.#onOpen = onOpen;
    this.switcher = segmented(["Library", "Structure"], { selected: 0, help: "Library or structure",
                                                          onChange: (i) => onSwitch(i === 0) });
    this.element = el("section.card.structure.library", { hidden: true, "aria-label": "Library" }, [
      this.switcher.element,
      el("p.note", { text: "Files you open are kept in this browser, to open again at once." }),
      pill({ title: "Open a file…", help: "Open a STEP, STL or cache file (⌘O)", onPress: onChoose }),
      this.#list,
    ]);
  }

  async refresh() {
    const entries = await libraryEntries();
    this.#list.replaceChildren(...entries.map((entry) => el("button.entry", {
      type: "button", title: "Open; ⌘-click adds it to what is shown",
      onclick: async (e) => this.#onOpen(await entry.file(), `${entry.name}`, e.metaKey || e.ctrlKey),
    }, [el("span.name", { text: entry.name }),
        el("span.size", { text: `${(entry.size / 1048576).toFixed(entry.size < 10485760 ? 1 : 0)} MB` })])));
    if (entries.length === 0) this.#list.append(el("p.empty", { text: "Nothing kept yet." }));
  }
}
