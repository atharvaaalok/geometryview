// The engine in the page: loads the WebAssembly module, answers its host calls, streams files into
// it, draws the overlay's text, and wraps its geo_web_* functions. Framework-free; everything runs
// on the main thread, as the engine does.

import createGeometry from "./build/geometry-web.mjs";

// GeoEventKind, as engine.h numbers them.
export const Events = Object.freeze({
  Loading: 0, SceneReady: 1, Failed: 2, Progress: 3, ModelReady: 4, SelectionChanged: 5,
  HoverChanged: 6, LooksChanged: 7, VisibilityChanged: 8,
});
export const Actions = Object.freeze({
  Fit: 0, ClearSelection: 1, ToggleProjection: 2, ShowAll: 3, UnghostAll: 4, Isolate: 5,
  HideSelection: 6, GhostSelection: 7, Undo: 8, Redo: 9, ViewFront: 10, ViewBack: 11,
  ViewLeft: 12, ViewRight: 13, ViewTop: 14, ViewBottom: 15, ViewIso: 16, FitSelection: 17,
  ToggleSeeThrough: 18,
});
export const Pointer = Object.freeze({ Move: 0, Down: 1, Up: 2, Scroll: 3, Magnify: 4, Leave: 5,
                                       Cancel: 6 });
export const Buttons = Object.freeze({ None: 0, Left: 1, Right: 2, Middle: 3 });
export const Modifiers = Object.freeze({ Shift: 1, Control: 2, Option: 4, Command: 8, Space: 16 });
export const NodeKinds = Object.freeze({ Assembly: 0, Part: 1, Body: 2 });
export const NodeActions = Object.freeze({ Hide: 0, Show: 1, Ghost: 2, Unghost: 3, Isolate: 4 });
export const Tools = Object.freeze({ Select: 0, Hide: 1, Ghost: 2 });

// The overlay atlas's layout, as gpu_types.h fixes it.
const kAtlasWidth = 512, kAtlasHeight = 6 * 48 + 48;
const kLabelWidth = 144, kLabelHeight = 48, kGlyphWidth = 28, kGlyphHeight = 48;
const kAtlasGlyphs = " 0123456789.kmu";

// Bytes handed to the engine at a time, through one staging buffer it owns.
const kStage = 1 << 20;

export class Engine {
  // `canvas` is drawn into; `handlers` takes the engine's events: { event(kind, document, node,
  // done, total, message), framesRunning(running) }.
  static async start(canvas, handlers, { phone = false } = {}) {
    const engine = new Engine(canvas, handlers, phone);
    await engine.#load();
    return engine;
  }

  #module = null;
  #canvas;
  #handlers;
  #sources = new Map(); // what each open path streams from
  #imports = new Map(); // token to its stream's state
  #nextSource = 1;
  #stage = 0;

  constructor(canvas, handlers, phone) {
    this.#canvas = canvas;
    this.#handlers = handlers;
    this.phone = phone;
  }

  async #load() {
    const phone = this.phone;
    const module = await createGeometry();
    this.#module = module;
    const ready = new Promise((resolve, reject) => {
      module.geoHost = {
        ready: () => resolve(),
        failed: (message) => reject(new Error(message)),
        runFrames: (running) => this.#handlers.framesRunning?.(running),
        // WebAssembly hands unsigned values over as signed ones.
        // A throw here would unwind through the engine, so the page's errors stop at this edge.
        event: (kind, document, node, done, total, message) => {
          try {
            this.#handlers.event?.(kind >>> 0, document >>> 0, node >>> 0, done >>> 0, total >>> 0, message);
          } catch (error) {
            reportError(error);
          }
        },
        startImport: (token, path) => this.#startImport(token, path),
        cancelImport: (token) => this.#imports.get(token)?.cancel(),
        snapshot: (pointer, width, height, bgra) => this.#snapshotArrived(pointer, width, height, bgra),
      };
    });
    const { width, height, scale } = this.#size();
    if (!this.#canvas.id) this.#canvas.id = "viewport";
    const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
    this.#withString(`#${this.#canvas.id}`, (selector) => module._geo_web_start(
        selector, width, height, scale, (reduceMotion ? 1 : 0) | (phone ? 2 : 0)));
    await ready;
    this.#stage = module._geo_web_staging(kStage);
    await this.#drawAtlas();
  }

  // ---- Sizes and input ---------------------------------------------------------------------

  #size() {
    const scale = this.scale;
    const rect = this.#canvas.getBoundingClientRect();
    const width = Math.max(1, Math.round(rect.width * scale));
    const height = Math.max(1, Math.round(rect.height * scale));
    this.#canvas.width = width;
    this.#canvas.height = height;
    return { width, height, scale };
  }

  // Pixels per CSS pixel. Phones draw at 2 at most: the third costs more than it shows.
  get scale() {
    return this.phone ? Math.min(devicePixelRatio, 2) : devicePixelRatio;
  }

  resize() {
    const { width, height, scale } = this.#size();
    this.#module._geo_web_resize(width, height, scale);
  }

  setInsets(left, top, right, bottom) {
    this.#module._geo_web_set_insets(left, top, right, bottom);
  }

  pointer(kind, button, x, y, delta = 0, modifiers = 0, timestamp = performance.now() / 1000) {
    this.#module._geo_web_pointer(kind, button, x, y, delta, modifiers, timestamp);
  }

  perform(action) { this.#module._geo_web_perform(action); }
  setSelectionMode(mode) { this.#module._geo_web_set_selection_mode(mode); }
  setTool(tool) { this.#module._geo_web_set_tool(tool); }
  setLook({ style, colorMode, ghostStyle, castShadow, grain, wobble }) {
    this.#module._geo_web_set_look(style, colorMode, ghostStyle, castShadow ? 1 : 0, grain, wobble);
  }
  // `cuts` is three { enabled, position, flip }.
  setSection(cuts) {
    let bits = 0;
    cuts.forEach((cut, axis) => {
      if (cut.enabled) bits |= 1 << axis;
      if (cut.flip) bits |= 16 << axis;
    });
    this.#module._geo_web_set_section(bits, cuts[0].position, cuts[1].position, cuts[2].position);
  }
  selectNode(document, node, extend) { this.#module._geo_web_select_node(document, node, extend ? 1 : 0); }
  hoverNode(document, node) { this.#module._geo_web_hover_node(document, node); }
  nodeAction(document, node, action) { this.#module._geo_web_node_action(document, node, action); }
  requestFrame() { this.#module._geo_web_request_frame(); }
  // Looks at what is shown from a direction, in degrees around Z and above the XY plane.
  setView(azimuth, elevation, perspective) {
    this.#module._geo_web_set_view(azimuth, elevation, perspective ? 1 : 0);
  }

  // The latest GPU error the engine saw, for diagnosing.
  lastError() { return this.#module.UTF8ToString(this.#module._geo_web_last_error()); }

  // The scene's bounds and face count.
  sceneInfo() {
    const m = this.#module;
    const out = m._malloc(8 * 8);
    m._geo_web_scene_info(out);
    const v = new Float64Array(m.HEAPU8.buffer, out, 8).slice();
    m._free(out);
    return { min: [v[0], v[1], v[2]], max: [v[3], v[4], v[5]], faces: v[6], triangles: v[7] };
  }

  // ---- Files -------------------------------------------------------------------------------

  // Opens a cache file from a URL or a File, replacing what is open or added to it. `name` is
  // what the interface calls it.
  open(source, name, { add = false } = {}) {
    const path = `${this.#nextSource++}/${name}`;
    this.#sources.set(path, source);
    this.#withString(path, (pointer) => this.#module._geo_web_open(pointer, add ? 1 : 0));
  }

  async #startImport(token, path) {
    const source = this.#sources.get(path);
    const module = this.#module;
    let cancelled = false;
    let reader = null;
    this.#imports.set(token, { cancel: () => { cancelled = true; reader?.cancel(); } });
    try {
      let body = source, size = 0;
      if (typeof source === "string") {
        const response = await fetch(source);
        body = response.body;
        size = Number(response.headers.get("content-length") ?? 0);
      } else if (source instanceof Blob) {
        body = source.stream();
        size = source.size;
      }
      // The file's real size bounds what its header may ask to be placed.
      if (size > 0) module._geo_web_stream_size(token, size);
      reader = body.getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (cancelled) return;
        if (done) break;
        for (let at = 0; at < value.length; at += kStage) {
          const piece = value.subarray(at, Math.min(value.length, at + kStage));
          module.HEAPU8.set(piece, this.#stage);
          if (module._geo_web_stream_chunk(token, this.#stage, piece.length) <= 0) {
            reader.cancel();
            return;
          }
        }
      }
      module._geo_web_stream_end(token, 0);
    } catch (error) {
      if (!cancelled) {
        this.#withString(`The file could not be read: ${error.message}`,
                         (message) => module._geo_web_stream_end(token, message));
      }
    } finally {
      this.#imports.delete(token);
    }
  }

  // The view as drawn, with all its samples and the overlay: { width, height, pixels } with
  // pixels as RGBA, or null if nothing is open.
  snapshot() {
    return new Promise((resolve) => {
      this.#snapshots.push(resolve);
      if (this.#snapshots.length === 1) this.#module._geo_web_snapshot();
    });
  }

  #snapshots = [];

  #snapshotArrived(pointer, width, height, bgra) {
    const resolve = this.#snapshots.shift();
    let image = null;
    if (pointer !== 0 && width > 0) {
      const pixels = this.#module.HEAPU8.slice(pointer, pointer + width * height * 4);
      if (bgra) {
        for (let p = 0; p < pixels.length; p += 4) {
          const b = pixels[p];
          pixels[p] = pixels[p + 2];
          pixels[p + 2] = b;
        }
      }
      image = { width, height, pixels };
    }
    resolve?.(image);
    if (this.#snapshots.length > 0) this.#module._geo_web_snapshot();
  }

  // ---- What the engine publishes -----------------------------------------------------------

  // A document's tree: rows of { parent, end, firstOccurrence, occurrenceEnd, name, kind }.
  model(document) {
    const m = this.#module;
    const count = m._geo_web_model_node_count(document);
    if (count === 0) return null;
    const words = new Uint32Array(m.HEAPU8.buffer, m._geo_web_model_nodes(document), count * 8);
    const strings = new Uint8Array(m.HEAPU8.buffer, m._geo_web_model_strings(document),
                                   m._geo_web_model_strings_size(document));
    const decoder = new TextDecoder();
    const nodes = new Array(count);
    for (let n = 0; n < count; ++n) {
      const w = n * 8;
      nodes[n] = {
        parent: words[w] === 0xFFFFFFFF ? -1 : words[w],
        end: words[w + 1],
        firstOccurrence: words[w + 2],
        occurrenceEnd: words[w + 3],
        name: decoder.decode(strings.subarray(words[w + 4], words[w + 4] + words[w + 5])),
        kind: words[w + 6],
      };
    }
    return nodes;
  }

  // One NodeVisibility per row: 0 shown, 1 hidden, 2 ghosted, 3 mixed.
  nodeStates(document) {
    const m = this.#module;
    const count = m._geo_web_node_state_count(document);
    return count === 0 ? new Uint8Array(0)
                       : new Uint8Array(m.HEAPU8.buffer, m._geo_web_node_states(document), count).slice();
  }

  // Each occurrence's swatch, RGBA8 with red in the low byte, in global order, and each
  // document's first occurrence.
  swatches() {
    const m = this.#module;
    const at = (pointer, count) => count === 0 ? new Uint32Array(0)
                                               : new Uint32Array(m.HEAPU8.buffer, pointer, count).slice();
    return { colors: at(m._geo_web_swatches(), m._geo_web_swatch_count()),
             bases: at(m._geo_web_swatch_bases(), m._geo_web_swatch_base_count()) };
  }

  summary() {
    const m = this.#module;
    const out = m._malloc(7 * 4);
    m._geo_web_summary(out);
    const v = new Uint32Array(m.HEAPU8.buffer, out, 7).slice();
    m._free(out);
    return { occurrences: v[0], faces: v[1], document: v[2], node: v[3] === 0xFFFFFFFF ? -1 : v[3],
             mode: v[4], selected: v[5], hidden: v[6] };
  }

  stats() {
    const m = this.#module;
    const out = m._malloc(9 * 8);
    m._geo_web_stats(out);
    const v = new Float64Array(m.HEAPU8.buffer, out, 9).slice();
    m._free(out);
    return { frames: v[0], settledFrames: v[1], gpuMs: v[2], bufferMB: v[3], targetMB: v[4],
             triangles: v[5], edgeSegments: v[6], looping: v[7] !== 0, hovered: v[8],
             heapMB: m.HEAPU8.length / 1048576 };
  }

  // ---- The overlay's text ------------------------------------------------------------------

  // The view cube's labels and the scale bar's glyphs, drawn as the Mac draws them with Core
  // Text: IBM Plex Mono SemiBold, white on black, each centred on its cap height in its cell.
  async #drawAtlas() {
    const family = "Geometry Plex Mono";
    try {
      const face = new FontFace(family, `url(${new URL("./fonts/IBMPlexMono-SemiBold.ttf", import.meta.url)})`,
                                { weight: "600" });
      document.fonts.add(await face.load());
    } catch {
      // The browser's own monospace stands in.
    }
    const canvas = new OffscreenCanvas(kAtlasWidth, kAtlasHeight);
    const context = canvas.getContext("2d", { willReadFrequently: true });
    context.fillStyle = "#000";
    context.fillRect(0, 0, kAtlasWidth, kAtlasHeight);
    context.fillStyle = "#fff";
    context.textBaseline = "alphabetic";
    const draw = (text, size, left, width, top, height) => {
      context.font = `600 ${size}px "${family}", Menlo, monospace`;
      const capHeight = context.measureText("H").actualBoundingBoxAscent;
      const x = left + (width - context.measureText(text).width) / 2;
      context.fillText(text, x, top + (height + capHeight) / 2);
    };
    ["FRONT", "BACK", "LEFT", "RIGHT", "TOP", "BOTTOM"].forEach((label, face) => {
      draw(label, 23, 0, kLabelWidth, face * kLabelHeight, kLabelHeight);
    });
    // The glyph strip, drawn for a 2x display: 11 points is 22 pixels.
    [...kAtlasGlyphs].forEach((c, cell) => {
      draw(c === "u" ? "µ" : c, 22, cell * kGlyphWidth, kGlyphWidth, 6 * kLabelHeight, kGlyphHeight);
    });
    context.font = `600 22px "${family}", Menlo, monospace`;
    const advance = context.measureText("0").width;
    const rgba = context.getImageData(0, 0, kAtlasWidth, kAtlasHeight).data;
    const m = this.#module;
    const pixels = m._malloc(kAtlasWidth * kAtlasHeight);
    const coverage = new Uint8Array(m.HEAPU8.buffer, pixels, kAtlasWidth * kAtlasHeight);
    for (let p = 0; p < coverage.length; ++p) coverage[p] = rgba[p * 4];
    m._geo_web_set_atlas(pixels, kAtlasWidth * kAtlasHeight, advance);
    m._free(pixels);
  }

  // Calls `use` with `text` in the module's memory, which the engine copies if it keeps it.
  #withString(text, use) {
    const m = this.#module;
    const size = m.lengthBytesUTF8(text) + 1;
    const pointer = m._malloc(size);
    m.stringToUTF8(text, pointer, size);
    try {
      return use(pointer);
    } finally {
      m._free(pointer);
    }
  }
}
