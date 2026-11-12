// The gallery's layout core (Engine/include/gallery.h), in the engine's module: the folder tree, the
// folder in view, and its display list, the same the Mac paints. Engine/browser/web_gallery.cpp
// carries it across; this reads the items from the module's memory.

export const Kinds = Object.freeze({ Rect: 0, Text: 1, Picture: 2, Line: 3 });
export const Flags = Object.freeze({ Dashed: 1, Hatched: 2, Sticker: 4, Fading: 8, Fixed: 16, Sheet: 32, Stale: 64, Lost: 128,
                                     Ruler: 256 });
export const Targets = Object.freeze({ None: 0, Folder: 1, File: 2, More: 3, Peek: 4, Reconnect: 5 });
export const Actions = Object.freeze({ None: 0, Handled: 1, EnterFolder: 2, OpenFile: 3, Peek: 4, Leave: 5,
                                       Reconnect: 6 });
export const Keys = Object.freeze({ Left: 0, Right: 1, Up: 2, Down: 3, Enter: 4, Space: 5, Escape: 6, FolderUp: 7 });
export const Views = Object.freeze({ Plates: 0, Sheet: 1, Ladders: 2, Atlas: 3, Catalogue: 4 });
export const ViewNames = Object.freeze(["Plates", "Sheet", "Ladders", "Atlas", "Catalogue"]);
export const Scales = Object.freeze({ Fit: 0, Folder: 1, True: 2 });
export const Sorts = Object.freeze({ Name: 0, Size: 1 });
export const Transitions = Object.freeze({ None: 0, Switch: 1, Into: 2, Out: 3 });
export const TrackKinds = Object.freeze({ Fly: 0, FadeIn: 1 });

const kItemSize = 116; // GeoGalleryItem: 29 four-byte fields
const kNone = 0xffffffff;

/** A colour from the core: straight sRGB, red in the low byte, as CSS. */
export function cssColour(rgba) {
  if (!rgba) return "transparent";
  const r = rgba & 0xff, g = (rgba >>> 8) & 0xff, b = (rgba >>> 16) & 0xff, a = (rgba >>> 24) / 255;
  return a >= 1 ? `rgb(${r} ${g} ${b})` : `rgb(${r} ${g} ${b} / ${Math.round(a * 1000) / 1000})`;
}

export class GalleryCore {
  #m;
  #g;
  #scratch;
  #items = null;
  #text = "";
  #textBytes = null;

  constructor(module) {
    this.#m = module;
    if (module._gv_gallery_item_size() !== kItemSize) throw new Error("The gallery's items are not the size the page reads.");
    this.#g = module._gv_gallery_create();
    this.#scratch = module._malloc(256);
  }

  destroy() {
    this.#m._gv_gallery_destroy(this.#g);
    this.#m._free(this.#scratch);
  }

  #string(text, use) {
    const m = this.#m;
    const size = m.lengthBytesUTF8(text) + 1;
    const pointer = m._malloc(size);
    m.stringToUTF8(text, pointer, size);
    try {
      return use(pointer);
    } finally {
      m._free(pointer);
    }
  }

  /** Replaces the tree with a walk's entries: [{ path, folder, bytes, modified }]. */
  setEntries(title, entries) {
    const m = this.#m;
    const n = entries.length;
    const paths = entries.map((e) => e.path).join("\n");
    const folders = m._malloc(Math.max(1, n));
    const bytes = m._malloc(Math.max(8, n * 8));
    const modified = m._malloc(Math.max(8, n * 8));
    // Each call may grow the heap, which detaches views made before it: views after.
    this.#string(paths, (p) => this.#string(title, (t) => {
      m.HEAPU8.set(entries.map((e) => (e.folder ? 1 : 0)), folders);
      new Float64Array(m.HEAPU8.buffer, bytes, n).set(entries.map((e) => e.bytes ?? 0));
      new Float64Array(m.HEAPU8.buffer, modified, n).set(entries.map((e) => e.modified ?? 0));
      m._gv_gallery_set_entries(this.#g, t, p, folders, bytes, modified, n);
    }));
    m._free(folders);
    m._free(bytes);
    m._free(modified);
    this.#items = null;
  }

  get fileCount() { return this.#m._gv_gallery_file_count(this.#g); }
  path(file) { return this.#m.UTF8ToString(this.#m._gv_gallery_file_path(this.#g, file)); }
  fileAt(path) {
    const file = this.#string(path, (p) => this.#m._gv_gallery_file_at(this.#g, p)) >>> 0;
    return file === 0xffffffff ? null : file;
  }

  /** Facts as the library measured them: { size: [x, y, z], hasBox, estimated, parts, product, triangles }. */
  setFacts(file, f) {
    this.#string(f.product ?? "", (p) =>
      this.#m._gv_gallery_set_facts(this.#g, file, f.size[0], f.size[1], f.size[2], f.hasBox ? 1 : 0,
                                    f.estimated ? 1 : 0, f.parts ?? 0, p, f.triangles ?? 0));
  }

  /** A picture's note: { width, height, mmPerPixel, part: [x, y, w, h], ink: [x, y, w, h] }. */
  setPicture(file, p) {
    const m = this.#m;
    const floats = new Float32Array(m.HEAPU8.buffer, this.#scratch, 8);
    floats.set([...p.part, ...p.ink]);
    m._gv_gallery_set_picture(this.#g, file, p.width, p.height, p.mmPerPixel, this.#scratch, this.#scratch + 16);
  }

  setFailure(file, message) {
    this.#string(message || "This file could not be read.", (p) => this.#m._gv_gallery_set_failure(this.#g, file, p));
  }

  /** The view as a poster, printed with `date` in its foot. */
  setPoster(poster, date = "") {
    this.#string(date, (d) => this.#m._gv_gallery_set_poster(this.#g, poster ? 1 : 0, d));
  }
  get poster() { return this.#m._gv_gallery_poster(this.#g) !== 0; }

  /** The last layout's lanes, which scroll sideways: [{ id, rect: [x, y, w, h], contentWidth }]. */
  get lanes() {
    const m = this.#m;
    const n = m._gv_gallery_lanes(this.#g, 0, 0);
    if (n === 0) return [];
    const out = m._malloc(n * 24);
    m._gv_gallery_lanes(this.#g, out, n);
    const v = new Float32Array(m.HEAPU8.buffer, out, n * 6).slice();
    m._free(out);
    const lanes = [];
    for (let i = 0; i < n; i++) {
      lanes.push({ id: v[i * 6], rect: [v[i * 6 + 1], v[i * 6 + 2], v[i * 6 + 3], v[i * 6 + 4]], contentWidth: v[i * 6 + 5] });
    }
    return lanes;
  }
  setLaneScroll(lane, x) { this.#m._gv_gallery_set_lane_scroll(this.#g, lane, x); }
  /** The selection's lane and the scroll that shows it, or null if it shows or is in none. */
  revealLane() {
    if (!this.#m._gv_gallery_reveal_lane(this.#g, this.#scratch)) return null;
    const v = new Float32Array(this.#m.HEAPU8.buffer, this.#scratch, 2);
    return { lane: v[0], x: v[1] };
  }
  /** Ladders' scale: each strip its own, or one shared. */
  setLadders(shared) { this.#m._gv_gallery_set_ladders(this.#g, shared ? 1 : 0); }

  /** The folder can no longer be read: what was last seen, faded, with Reconnect. */
  setLost(lost) { this.#m._gv_gallery_set_lost(this.#g, lost ? 1 : 0); }

  /** A file being read, 0 to 1, or negative once it is not. */
  setReading(file, fraction) { this.#m._gv_gallery_set_reading(this.#g, file, fraction); }
  /** A file changed on disk: a Δ until seen, its picture faded until a new one comes. */
  setChanged(file, changed) { this.#m._gv_gallery_set_changed(this.#g, file, changed ? 1 : 0); }
  setStyle(style) { this.#m._gv_gallery_set_style(this.#g, style); }
  setViewport(width, height) { this.#m._gv_gallery_set_viewport(this.#g, width, height); }
  setScroll(top) { this.#m._gv_gallery_set_scroll(this.#g, top); }
  setFolder(path) { return this.#string(path, (p) => this.#m._gv_gallery_set_folder(this.#g, p)) !== 0; }
  get folder() { return this.#m.UTF8ToString(this.#m._gv_gallery_folder(this.#g)); }
  get title() { return this.#m.UTF8ToString(this.#m._gv_gallery_folder_title(this.#g)); }
  get subtitle() { return this.#m.UTF8ToString(this.#m._gv_gallery_folder_subtitle(this.#g)); }
  get contentHeight() { return this.#m._gv_gallery_content_height(this.#g); }
  get picturePixels() { return this.#m._gv_gallery_picture_pixels(this.#g); }

  /** Lays out if anything changed; returns whether it did. */
  layout() {
    const changed = this.#m._gv_gallery_layout(this.#g) !== 0;
    if (changed) this.#items = null;
    return changed;
  }

  /** The display list, decoded once a layout. */
  get items() {
    if (this.#items) return this.#items;
    const m = this.#m;
    const n = m._gv_gallery_item_count(this.#g);
    const base = m._gv_gallery_items(this.#g);
    const view = new DataView(m.HEAPU8.buffer, base, n * kItemSize);
    const text = m._gv_gallery_text(this.#g);
    const items = new Array(n);
    let end = 0;
    for (let i = 0; i < n; i++) {
      const o = i * kItemSize;
      const u = (k) => view.getUint32(o + k * 4, true);
      const f = (k) => view.getFloat32(o + k * 4, true);
      items[i] = {
        kind: u(0), flags: u(1), x: f(2), y: f(3), width: f(4), height: f(5),
        fill: u(6), stroke: u(7), shadow: u(8), lineWidth: f(9), radius: f(10),
        text: u(11), textLength: u(12), fontSize: f(13), tracking: f(14), weight: u(15), align: u(16),
        baseline: f(17), file: u(18), targetKind: u(19), target: u(20), key: u(21),
        group: u(22), line: f(23), column: u(24), columnLine: f(25),
        stickyTop: f(26), stickyUntil: f(27), lane: u(28),
      };
      end = Math.max(end, items[i].text + items[i].textLength);
    }
    this.#textBytes = m.HEAPU8.slice(text, text + end);
    this.#items = items;
    return items;
  }

  /** An item's text. */
  text(item) {
    return new TextDecoder().decode(this.#textBytes.subarray(item.text, item.text + item.textLength));
  }

  hit(x, y) {
    const m = this.#m;
    m._gv_gallery_hit(this.#g, x, y, this.#scratch);
    const out = new Uint32Array(m.HEAPU8.buffer, this.#scratch, 3);
    return { kind: out[0], target: out[1], key: out[2] };
  }

  hover(kind, target) { return this.#m._gv_gallery_hover(this.#g, kind, target) !== 0; }

  #action(call) {
    call(this.#scratch);
    const out = new Uint32Array(this.#m.HEAPU8.buffer, this.#scratch, 2);
    return { kind: out[0], target: out[1] };
  }

  click(kind, target, twice) {
    return this.#action((out) => this.#m._gv_gallery_click(this.#g, kind, target, twice ? 1 : 0, out));
  }

  key(key) {
    return this.#action((out) => this.#m._gv_gallery_key(this.#g, key, out));
  }

  // ---- Views, search, Peek and motion ----

  /** The views that may be shown, a bit each (1 << Views.Plates ...). */
  setViews(mask) { this.#m._gv_gallery_set_views(this.#g, mask); }
  get views() { return this.#m._gv_gallery_views(this.#g) >>> 0; }
  /** Shows a view if it is enabled; the folder in view remembers it. */
  setView(view) { return this.#m._gv_gallery_set_view(this.#g, view) !== 0; }
  get view() { return this.#m._gv_gallery_view(this.#g); }
  /** Sheet's scale (Scales), cell size in points and order (Sorts). */
  setSheet(scale, cell, sort) { this.#m._gv_gallery_set_sheet(this.#g, scale, cell, sort); }

  setSearch(query) { this.#string(query, (p) => this.#m._gv_gallery_set_search(this.#g, p)); }
  get search() { return this.#m.UTF8ToString(this.#m._gv_gallery_search(this.#g)); }
  /** How many files the search found, or null with no search. */
  get matches() {
    const n = this.#m._gv_gallery_matches(this.#g) >>> 0;
    return n === kNone ? null : n;
  }

  peek(file) { this.#m._gv_gallery_peek(this.#g, file ?? kNone); }
  get peeking() {
    const file = this.#m._gv_gallery_peeking(this.#g) >>> 0;
    return file === kNone ? null : file;
  }

  /** The selected file, or null. */
  get selected() {
    const file = this.#m._gv_gallery_selected(this.#g) >>> 0;
    return file === kNone ? null : file;
  }

  /** Where to scroll so the selection shows, or null if it shows already. */
  get reveal() {
    const top = this.#m._gv_gallery_reveal(this.#g);
    return top < 0 ? null : top;
  }

  /** What the last layout plays against the one before: { kind, rect: [x, y, w, h], duration, fadeOut }. */
  get transition() {
    const m = this.#m;
    m._gv_gallery_transition(this.#g, this.#scratch);
    const v = new Float32Array(m.HEAPU8.buffer, this.#scratch, 7);
    return { kind: v[0], rect: [v[1], v[2], v[3], v[4]], duration: v[5], fadeOut: v[6] };
  }

  /** The last layout's tracks: [{ item, kind, from: [x, y, w, h], delay, duration }]. */
  get tracks() {
    const m = this.#m;
    const n = m._gv_gallery_tracks(this.#g, 0, 0);
    if (n === 0) return [];
    const out = m._malloc(n * 32);
    m._gv_gallery_tracks(this.#g, out, n);
    const v = new Float32Array(m.HEAPU8.buffer, out, n * 8).slice();
    m._free(out);
    const tracks = new Array(n);
    for (let i = 0; i < n; i++) {
      const o = i * 8;
      tracks[i] = { item: v[o], kind: v[o + 1], from: [v[o + 2], v[o + 3], v[o + 4], v[o + 5]],
                    delay: v[o + 6], duration: v[o + 7] };
    }
    return tracks;
  }

  /** The files to draw, in the library's bands: [{ file, band, order, folderColor }]. */
  wanted() {
    const m = this.#m;
    const n = m._gv_gallery_wanted(this.#g, 0, 0);
    const out = m._malloc(Math.max(32, n * 32));
    m._gv_gallery_wanted(this.#g, out, n);
    const values = new Float64Array(m.HEAPU8.buffer, out, n * 4).slice();
    m._free(out);
    const wants = new Array(n);
    for (let i = 0; i < n; i++) {
      wants[i] = { file: values[i * 4], band: values[i * 4 + 1], order: values[i * 4 + 2], folderColor: values[i * 4 + 3] };
    }
    return wants;
  }

  /** The alignment groups painted more than `tolerance` apart: { over: [{ group, column, spread, items }], checked }. */
  check(lines, columns, tolerance = 0.5) {
    const m = this.#m;
    const n = lines.length;
    const l = m._malloc(Math.max(4, n * 4)), c = m._malloc(Math.max(4, n * 4));
    const out = m._malloc(64 * 32), checked = m._malloc(4);
    new Float32Array(m.HEAPU8.buffer, l, n).set(lines);
    new Float32Array(m.HEAPU8.buffer, c, n).set(columns);
    const over = m._gv_gallery_check(this.#g, l, c, tolerance, out, 64, checked);
    const values = new Float64Array(m.HEAPU8.buffer, out, Math.min(over, 64) * 4).slice();
    const count = new Uint32Array(m.HEAPU8.buffer, checked, 1)[0];
    for (const p of [l, c, out, checked]) m._free(p);
    const groups = [];
    for (let i = 0; i < values.length / 4; i++) {
      groups.push({ group: values[i * 4], column: values[i * 4 + 1] === 1, spread: values[i * 4 + 2], items: values[i * 4 + 3] });
    }
    return { over: groups, checked: count };
  }

  /** A style's sheet: { top, bottom, vignette, grid, gridAlpha, gridPitch, dark }. */
  sheet(style) {
    const m = this.#m;
    m._gv_style_sheet(style, this.#scratch);
    const v = new Float32Array(m.HEAPU8.buffer, this.#scratch, 13);
    return { top: [v[0], v[1], v[2]], bottom: [v[3], v[4], v[5]], vignette: v[6], grid: [v[7], v[8], v[9]],
             gridAlpha: v[10], gridPitch: v[11], dark: v[12] === 1 };
  }
}
