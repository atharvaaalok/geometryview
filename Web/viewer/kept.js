// What this browser keeps of its imports, in the origin-private file system under library2/: each
// file's cache file, its note, its pictures, and, for a STEP file that cannot be read again from
// a connected folder, its source, for refining. Each is named by a key, a hash of where the file
// came from, its path there, its size, its time and the cache version: two files of one name in
// two folders never meet, and a changed file is a new key.
//
//   library2/blobs/<key>.geoblob     the cache file
//   library2/sources/<key>.source    a STEP file's source, when it cannot be had again
//   library2/thumbs/<key>.<T>.png    its pictures, one per drawing spec T, each with a .json note
//   library2/notes/<key>.json        the note, written last: an entry without one is unfinished
//
// What is kept is evicted least recently used first, to a budget from the browser's storage
// estimate. The first library/ folder, named by bare file names, is still read until its files are
// imported again.

export const kRoot = "library2";
const kNote = "notes", kBlobs = "blobs", kSources = "sources", kThumbs = "thumbs";

/**
 * The key of a file: the first 128 bits of SHA-256 over where it came from (a connected folder's
 * name, or "file" for one opened on its own), its path there, its size, its modification time and
 * the cache version, as 32 hex digits.
 */
export async function keyOf({ source, path, size, modified, version }) {
  const text = JSON.stringify([String(source), String(path), Number(size), Number(modified), Number(version)]);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
  return [...digest.subarray(0, 16)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** The share of the browser's quota each kind of entry may take. */
export const kBlobShare = 0.5;
export const kPictureShare = 0.02;

/**
 * Which entries to delete so that what is kept fits `budget` bytes: the least recently used first,
 * never those in `keep`. Notes are { key, bytes, usedAt }.
 */
export function evictions(notes, budget, keep = new Set()) {
  let total = notes.reduce((sum, note) => sum + (note.bytes ?? 0), 0);
  const dropped = [];
  for (const note of [...notes].sort((a, b) => (a.usedAt ?? 0) - (b.usedAt ?? 0))) {
    if (total <= budget) break;
    if (keep.has(note.key)) continue;
    dropped.push(note.key);
    total -= note.bytes ?? 0;
  }
  return dropped;
}

// ---- The origin-private file system ------------------------------------------------------------

async function folder(name) {
  const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(kRoot, { create: true });
  return root.getDirectoryHandle(name, { create: true });
}

async function readText(directory, name) {
  return (await (await directory.getFileHandle(name)).getFile()).text();
}

// Written whole through a writable stream, which replaces the file only when closed.
async function writeText(directory, name, text) {
  const writable = await (await directory.getFileHandle(name, { create: true })).createWritable();
  await writable.write(text);
  await writable.close();
}

/** A cache file's version, from its header: 8 bytes of magic, then a little-endian u32. */
export async function versionOf(blob) {
  const header = new DataView(await blob.slice(0, 12).arrayBuffer());
  return header.byteLength >= 12 ? header.getUint32(8, true) : -1;
}

/** An entry's note, or null if it is not kept whole. */
export async function noteOf(key) {
  try {
    return JSON.parse(await readText(await folder(kNote), `${key}.json`));
  } catch {
    return null;
  }
}

/**
 * The kept import of `key`: { blob, note }, or null if there is none, it is unfinished, or its
 * cache file is in another version than `version`. Marks it used.
 */
export async function keptImport(key, version = null) {
  const note = await noteOf(key);
  if (!note) return null;
  try {
    const blob = await (await (await folder(kBlobs)).getFileHandle(`${key}.geoblob`)).getFile();
    if (version !== null && (await versionOf(blob)) !== version) return null;
    await touch(key, note);
    return { blob, note };
  } catch {
    return null;
  }
}

/** The STEP source kept for `key`, or null if none was kept whole. */
export async function keptSource(key) {
  const note = await noteOf(key);
  if (!note?.sourceBytes) return null;
  try {
    const source = await (await (await folder(kSources)).getFileHandle(`${key}.source`)).getFile();
    return source.size === note.sourceBytes ? source : null;
  } catch {
    return null;
  }
}

/** Marks an entry as just used, so eviction keeps it longest. */
export async function touch(key, note = null) {
  note ??= await noteOf(key);
  if (!note) return;
  note.usedAt = Date.now();
  await writeText(await folder(kNote), `${key}.json`, JSON.stringify(note)).catch(() => {});
}

/** Deletes everything kept for `key`: its note first, so a reader never sees half an entry. */
export async function forget(key) {
  try {
    await (await folder(kNote)).removeEntry(`${key}.json`).catch(() => {});
    await (await folder(kBlobs)).removeEntry(`${key}.geoblob`).catch(() => {});
    await (await folder(kSources)).removeEntry(`${key}.source`).catch(() => {});
    const thumbs = await folder(kThumbs);
    for await (const name of thumbs.keys()) {
      if (name.startsWith(`${key}.`)) await thumbs.removeEntry(name).catch(() => {});
    }
  } catch {
    // No origin-private file system here: nothing is kept.
  }
}

/** Deletes everything kept of files named `name`, wherever they came from: for checks. */
export async function forgetNamed(name) {
  for (const note of await notes()) if (note.name === name) await forget(note.key);
}

/** Every finished entry's note. */
export async function notes() {
  const found = [];
  try {
    const directory = await folder(kNote);
    for await (const [name, handle] of directory.entries()) {
      if (handle.kind !== "file" || !name.endsWith(".json")) continue;
      try {
        found.push(JSON.parse(await (await handle.getFile()).text()));
      } catch {
        // Being written: not finished.
      }
    }
  } catch {
    // Nothing kept here.
  }
  return found;
}

/** The bytes kept imports may hold: a share of the browser's quota. */
export async function blobBudget() {
  const { quota = 0 } = (await navigator.storage?.estimate?.().catch(() => null)) ?? {};
  return quota > 0 ? quota * kBlobShare : 2 * 1024 ** 3;
}

/** Evicts kept imports to the budget, sparing `keep` (keys open or in view). Returns the keys dropped. */
export async function evict(keep = new Set(), budget = null) {
  budget ??= await blobBudget();
  const dropped = evictions(await notes(), budget, keep);
  for (const key of dropped) await forget(key);
  return dropped;
}

let persisted = false;
/** Asks once that what is kept not be cleared under storage pressure: once a folder is connected. */
export function persistOnce() {
  if (persisted) return;
  persisted = true;
  navigator.storage?.persist?.().catch(() => {});
}

// ---- Writing, from a worker --------------------------------------------------------------------

async function writeWhole(directory, name, bytes) {
  const access = await (await directory.getFileHandle(name, { create: true })).createSyncAccessHandle();
  access.truncate(0);
  for (let at = 0; at < bytes.length;) at += access.write(bytes.subarray(at), { at });
  access.flush();
  access.close();
}

/**
 * Keeps an import, from a worker, where sync access handles write a cache file without holding a
 * second copy: the cache file, then the note, last, which makes it whole.
 */
export async function keepImport(key, bytes, note) {
  await writeWhole(await folder(kBlobs), `${key}.geoblob`, bytes);
  const whole = { ...note, key, bytes: bytes.length, sourceBytes: 0, usedAt: Date.now() };
  await writeWhole(await folder(kNote), `${key}.json`, new TextEncoder().encode(JSON.stringify(whole)));
  return whole;
}

/**
 * Keeps a STEP file's source beside its kept import, from a worker, read in pieces, once the import
 * is done, since nothing waits for it; then notes it, so a copy cut short is never read.
 */
export async function keepSource(key, file) {
  const access = await (await (await folder(kSources)).getFileHandle(`${key}.source`, { create: true }))
    .createSyncAccessHandle();
  access.truncate(0);
  for (let at = 0; at < file.size; at += kSourcePiece) {
    const piece = new Uint8Array(await file.slice(at, at + kSourcePiece).arrayBuffer());
    for (let done = 0; done < piece.length;) done += access.write(piece.subarray(done), { at: at + done });
  }
  access.flush();
  access.close();
  const note = await noteOf(key);
  if (!note) return;
  note.sourceBytes = file.size;
  note.bytes += file.size;
  await writeWhole(await folder(kNote), `${key}.json`, new TextEncoder().encode(JSON.stringify(note)));
}
const kSourcePiece = 8 << 20;

// ---- Pictures and facts --------------------------------------------------------------------------

/** The key of a picture of a file: a hash of how it is drawn and the engine's version. */
export async function pictureKeyOf({ style, colorMode, pixels, pixelsPerPoint, lineScale, folderColor, version }) {
  const text = JSON.stringify([style, colorMode, pixels, pixelsPerPoint, lineScale, folderColor, version]);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
  return [...digest.subarray(0, 8)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** A file's facts and its picture kept under `key`: { facts, png, note } or null for each missing. */
export async function keptPicture(key, picture) {
  try {
    const thumbs = await folder(kThumbs);
    const facts = JSON.parse(await readText(thumbs, `${key}.facts.json`));
    let png = null, note = null;
    try {
      note = JSON.parse(await readText(thumbs, `${key}.${picture}.json`));
      png = await (await thumbs.getFileHandle(`${key}.${picture}.png`)).getFile();
    } catch {
      // Drawn in another style, or not at all.
    }
    return { facts, png, note };
  } catch {
    return null;
  }
}

/**
 * Keeps a picture, its note and the file's facts, from a worker: the PNG, then the facts, then the
 * note, last, which marks the picture whole.
 */
export async function keepPicture(key, picture, png, note, facts) {
  const thumbs = await folder(kThumbs);
  await writeWhole(thumbs, `${key}.${picture}.png`, new Uint8Array(await png.arrayBuffer()));
  if (facts) await writeWhole(thumbs, `${key}.facts.json`, new TextEncoder().encode(JSON.stringify(facts)));
  await writeWhole(thumbs, `${key}.${picture}.json`, new TextEncoder().encode(JSON.stringify(note)));
}
