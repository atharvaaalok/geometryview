// What the Library lists, as rows: the connected folders, each a tree read a level at a time as it
// is opened, then the files opened recently. Pure: the store gives it the folders, their listings
// and the recent files, and the card draws its rows. The Mac app's LibraryRows.swift does the
// same, and Launcher/test/library-rows.test.mjs checks this one.

export const kRecentLimit = 12;

export const RowKinds = Object.freeze({
  Section: "section", // "Folders" or "Recent"
  Folder: "folder",   // a connected folder (depth 1) or one inside it
  File: "file",       // a STEP or STL file in a folder
  Recent: "recent",   // a file opened before
  Message: "message", // a line of text where rows would be
});

export const Sections = Object.freeze({ Folders: "folders", Recent: "recent" });

const kModel = /\.(step|stp|stl)$/i;
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" });

// What a folder lists: its folders, then its STEP and STL files, each in the order people read
// numbers in names; hidden names left out. `entries` are { name, kind: "folder" | "file" }.
export function listed(entries) {
  const shown = entries.filter((e) => !e.name.startsWith(".") &&
                                      (e.kind === "folder" || kModel.test(e.name)));
  return shown.sort((a, b) => (a.kind === b.kind ? collator.compare(a.name, b.name)
                                                 : a.kind === "folder" ? -1 : 1));
}

// The key of a folder's listing, or a file's, by its connected folder and the path inside it.
export const keyOf = (root, path) => `${root}/${path}`;

// The rows, top to bottom. `state` is:
//   folders:   [{ id, name, state: "ready" | "reconnect" | "missing" | "visit" }]
//   listings:  Map of keyOf(folder, path) to { state: "reading" | "listed" | "failed", entries }
//   expanded:  Set of keyOf(folder, path) for the folders shown open
//   collapsed: Set of the Sections hidden
//   recent:    [{ key, name, where, openedAt }], newest first
//   open:      Set of the keys of the files shown now
// A row is { kind, depth, key, name, ... }.
export function rowsOf({ folders, listings, expanded, collapsed, recent, open = new Set() }) {
  const rows = [];
  const folding = !collapsed.has(Sections.Folders);
  rows.push({ kind: RowKinds.Section, depth: 0, key: Sections.Folders, name: "Folders",
              count: folders.length, expanded: folding });
  if (folding) {
    if (folders.length === 0) {
      rows.push({ kind: RowKinds.Message, depth: 1, key: "folders-empty",
                  text: "Connect a folder to browse it." });
    }
    for (const folder of folders) {
      const key = keyOf(folder.id, "");
      const usable = folder.state === "ready" || folder.state === "visit";
      const isOpen = usable && expanded.has(key);
      rows.push({ kind: RowKinds.Folder, depth: 1, key, root: folder.id, path: "", name: folder.name,
                  state: folder.state, expanded: isOpen, top: true });
      if (isOpen) addListing(rows, folder.id, "", 2, listings, expanded, open);
    }
  }
  const recentShown = !collapsed.has(Sections.Recent);
  rows.push({ kind: RowKinds.Section, depth: 0, key: Sections.Recent, name: "Recent",
              count: recent.length, expanded: recentShown });
  if (recentShown) {
    if (recent.length === 0) {
      rows.push({ kind: RowKinds.Message, depth: 1, key: "recent-empty", text: "Files you open appear here." });
    }
    for (const file of recent) {
      rows.push({ kind: RowKinds.Recent, depth: 1, key: `recent:${file.key}`, recentKey: file.key,
                  name: file.name, where: file.where, openedAt: file.openedAt, missing: file.missing,
                  open: open.has(file.key) });
    }
  }
  return rows;
}

function addListing(rows, root, path, depth, listings, expanded, open) {
  const listing = listings.get(keyOf(root, path));
  if (!listing || listing.state === "reading") {
    if (listing?.slow) rows.push({ kind: RowKinds.Message, depth, key: `${keyOf(root, path)}:reading`, text: "Reading…" });
    return;
  }
  if (listing.state === "failed") {
    rows.push({ kind: RowKinds.Message, depth, key: `${keyOf(root, path)}:failed`, text: "This folder could not be read." });
    return;
  }
  const entries = listed(listing.entries);
  if (entries.length === 0) {
    rows.push({ kind: RowKinds.Message, depth, key: `${keyOf(root, path)}:empty`, text: "No STEP or STL files here." });
  }
  for (const entry of entries) {
    const inner = path ? `${path}/${entry.name}` : entry.name;
    const key = keyOf(root, inner);
    if (entry.kind === "folder") {
      const isOpen = expanded.has(key);
      rows.push({ kind: RowKinds.Folder, depth, key, root, path: inner, name: entry.name, state: "ready",
                  expanded: isOpen, top: false });
      if (isOpen) addListing(rows, root, inner, depth + 1, listings, expanded, open);
    } else {
      rows.push({ kind: RowKinds.File, depth, key, root, path: inner, name: entry.name, open: open.has(key) });
    }
  }
}

// The recent files with `file` first: an entry with the same key moves up rather than repeating,
// and the oldest beyond the limit fall off. Returns { recent, dropped }.
export function remember(recent, file, limit = kRecentLimit) {
  const rest = recent.filter((r) => r.key !== file.key);
  const next = [file, ...rest];
  return { recent: next.slice(0, limit), dropped: next.slice(limit) };
}

// How long ago, in a few characters: "now", "5 min", "3 h", "2 d".
export function ageText(openedAt, now = Date.now()) {
  const minutes = Math.floor(Math.max(0, now - openedAt) / 60000);
  if (minutes < 1) return "now";
  if (minutes < 60) return `${minutes} min`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours} h`;
  return `${Math.floor(hours / 24)} d`;
}
