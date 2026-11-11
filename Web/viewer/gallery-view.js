// Paints the gallery's display list (gallery-core.js) with the DOM, over the style's sheet, and turns
// the pointer into the gallery's actions: the browser's counterpart of App/Sources/Gallery/
// GalleryView.swift. Everything it draws was placed by the layout core, so the Mac draws the same.
//
// Items are absolutely positioned elements, reused from one paint to the next by kind, and only
// those within a screen of the view are painted; scrolling within that moves nothing but the
// headings that stay at the top. Items fixed in the view (Peek) are painted over the content.
//
// The core says what a layout plays against the one before (gallery-core.js, transition and
// tracks); this plays it with Web Animations, the old view a copy fading out beneath the new.

import { Actions, cssColour, Flags, Kinds, Targets, TrackKinds, Transitions, Views } from "./gallery-core.js";

const kAscent = 1.025, kLine = 1.3; // IBM Plex Mono, in ems, as the core reckons
const kFaded = 0.22; // what a search did not find
const kStale = 0.45; // a picture being drawn again
const kLost = 0.35; // a folder that can no longer be read, as last seen
const kEase = "cubic-bezier(0.3, 0.7, 0.2, 1)";
const reduceMotion = () => globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches ?? false;

/** Where the content shows in the window: right of the Library card, above the toolbar. */
export const kContentInsets = Object.freeze({ top: 96, left: 340, bottom: 84, right: 18 });

export class GalleryView {
  element;
  #core;
  #on;
  #sheet;
  #stage;
  #clip;
  #content;
  #fixed;
  #ghost = null;
  #sheetBackground = "";
  #sheetSpec = null;
  #images = new Map(); // file -> object URL
  #details = new Map(); // file -> object URL of its detail picture, for where it is drawn large
  #painted = null; // { top, bottom } of what is painted
  #shownTop = 0; // the scroll last painted, where a copy of the old view is taken from
  #layers;
  #frames = new Map(); // item index -> element painted for it
  #sticky = []; // { node, item } of headings that stay at the top
  #lifted = new Map(); // item index -> how far a heading is moved down
  // Lanes, which scroll sideways on their own: id -> { rect, contentWidth }, their scroll, and a
  // layer each, its elements in a clipped box.
  #lanes = new Map();
  #lanesOf = null;
  #laneScroll = new Map();
  #laneLayers = new Map();
  #drag = null;
  // Peek's turntable: { file, frames (ImageBitmaps), ink: [x, y, w, h] holding every frame's }, the
  // frame shown, and the box and canvas it is drawn in, over Peek's picture area.
  #turntable = null;
  #turnFrame = 0;
  #turnBox;
  #turnCanvas;
  #style = 0;
  #insets = kContentInsets;
  #probes = false;

  /**
   * `on` takes action(action) for what a click or key asks of the page, and viewChanged() when what
   * is in view changed. With `probes`, each text carries a mark on its baseline, for the checks.
   */
  constructor(core, on, { probes = false } = {}) {
    this.#core = core;
    this.#on = on;
    this.#probes = probes;
    this.element = el("div", "gallery");
    this.#sheet = el("div", "gallery-sheet");
    this.#stage = el("div", "gallery-stage");
    this.#clip = el("div", "gallery-clip");
    this.#content = el("div", "gallery-content");
    this.#fixed = el("div", "gallery-fixed");
    this.#clip.append(this.#content);
    this.#stage.append(this.#clip, this.#fixed);
    this.#turnCanvas = document.createElement("canvas");
    this.#turnBox = el("div", "g-turn");
    this.#turnBox.style.cssText = "position: absolute; overflow: hidden; pointer-events: none";
    this.#turnCanvas.style.cssText = "position: absolute; left: 0; top: 0; width: 100%; height: 100%";
    this.#turnBox.append(this.#turnCanvas);
    this.#turnBox.hidden = true;
    this.#fixed.append(this.#turnBox);
    this.element.append(this.#sheet, this.#stage);
    const pools = () => ({ rect: [], text: [], picture: [], line: [] });
    this.#layers = { content: { node: this.#content, pools: pools() }, fixed: { node: this.#fixed, pools: pools() } };
    this.#clip.addEventListener("scroll", () => {
      this.#core.setScroll(this.#clip.scrollTop);
      this.#paint();
      this.#on.viewChanged?.();
    }, { passive: true });
    this.#clip.addEventListener("pointermove", (e) => this.#hover(e));
    this.#clip.addEventListener("pointerleave", () => {
      if (this.#core.hover(Targets.None, 0)) this.reload();
    });
    this.#clip.addEventListener("click", (e) => {
      if (this.#drag?.moved) {
        this.#drag = null; // a drag along a lane is not a click
        return;
      }
      this.#click(e, e.detail >= 2);
    });
    // A lane scrolls sideways: a sideways wheel or a shifted one over it, or a drag along it.
    this.#clip.addEventListener("wheel", (e) => {
      if (this.#core.view === Views.Atlas) {
        // The map: a modified wheel or a pinch zooms about the pointer, a plain one pans.
        e.preventDefault();
        const { x, y } = this.#point(e);
        if (e.ctrlKey || e.metaKey || e.altKey || e.deltaMode === 1) {
          this.#core.atlasZoom(Math.exp(-e.deltaY * (e.ctrlKey ? 0.012 : 0.002)), x, y);
        } else {
          this.#core.atlasPan(-e.deltaX, -e.deltaY);
        }
        this.#atlasMoved();
        return;
      }
      const lane = this.#laneAt(this.#point(e));
      if (!lane) return;
      const sideways = Math.abs(e.deltaX) > Math.abs(e.deltaY) || e.shiftKey;
      if (!sideways) return;
      e.preventDefault();
      this.#scrollLane(lane, (this.#laneScroll.get(lane.id) ?? 0) + (e.deltaX || e.deltaY));
    }, { passive: false });
    this.#clip.addEventListener("pointerdown", (e) => {
      if (e.button === 0 && this.#overTurntable(e)) {
        // A drag across Peek's turntable turns the part, a frame every 24 points.
        this.#drag = { turn: true, x: e.clientX, from: this.#turnFrame, moved: false };
        return;
      }
      if (this.#core.view === Views.Atlas && e.button === 0) {
        this.#drag = { atlas: true, x: e.clientX, y: e.clientY, moved: false };
        return;
      }
      const lane = this.#laneAt(this.#point(e));
      this.#drag = lane && e.button === 0 ? { lane, x: e.clientX, from: this.#laneScroll.get(lane.id) ?? 0, moved: false } : null;
    });
    this.#clip.addEventListener("pointermove", (e) => {
      const d = this.#drag;
      if (!d || !(e.buttons & 1)) return;
      if (d.turn) {
        if (Math.abs(e.clientX - d.x) >= 4) d.moved = true; // a turn is not a click
        this.#showTurnFrame(d.from + Math.round((e.clientX - d.x) / 24));
        return;
      }
      if (d.atlas) {
        // A drag pans the map.
        const dx = e.clientX - d.x, dy = e.clientY - d.y;
        if (!d.moved && Math.abs(dx) + Math.abs(dy) < 4) return;
        d.moved = true;
        d.x = e.clientX;
        d.y = e.clientY;
        this.#core.atlasPan(dx, dy);
        this.#atlasMoved();
        return;
      }
      if (!d.moved && Math.abs(e.clientX - d.x) < 4) return;
      d.moved = true;
      this.#scrollLane(d.lane, d.from - (e.clientX - d.x));
    });
    this.#place();
  }

  /** Where the content shows, in the window. */
  setInsets(insets) {
    this.#insets = insets;
    this.#place();
  }

  #place() {
    Object.assign(this.#stage.style, { left: `${this.#insets.left}px`, top: `${this.#insets.top}px`,
                                       right: `${this.#insets.right}px`, bottom: `${this.#insets.bottom}px` });
  }

  get contentArea() {
    return { width: this.#clip.clientWidth, height: this.#clip.clientHeight };
  }

  setStyle(style) {
    this.#style = style;
    this.#core.setStyle(style);
    this.#drawSheet();
    this.reload();
  }

  /** A file's picture, as a URL an image can show. */
  setImage(file, url) {
    const old = this.#images.get(file);
    if (old && old !== url) URL.revokeObjectURL(old);
    this.#images.set(file, url);
  }

  clearImages() {
    for (const url of [...this.#images.values(), ...this.#details.values()]) URL.revokeObjectURL(url);
    this.#images.clear();
    this.#details.clear();
  }

  scrollToTop() {
    // A new folder's lanes start at their beginnings.
    for (const id of this.#laneScroll.keys()) this.#core.setLaneScroll(id, 0);
    this.#laneScroll.clear();
    this.#clip.scrollTop = 0;
    this.#core.setScroll(0);
    this.#painted = null;
  }

  /**
   * Lays out again if anything changed, and paints what is in view, playing what the change asks.
   * With `reveal`, scrolls first so the selection shows: after a key or a switch of views.
   */
  reload({ reveal = false } = {}) {
    const { width, height } = this.contentArea;
    if (width === 0 || height === 0) return;
    this.#core.setViewport(width, height);
    const changed = this.#core.layout();
    let motion = null;
    if (changed) {
      this.#painted = null;
      const transition = this.#core.transition;
      if (transition.kind !== Transitions.None && !reduceMotion()) {
        motion = { transition, tracks: this.#core.tracks, ghost: this.#copyOfShown() };
      }
    }
    this.#content.style.height = `${Math.max(this.#core.contentHeight, height)}px`;
    const top = reveal ? this.#core.reveal : null;
    if (top !== null) {
      this.#clip.scrollTop = top;
      this.#core.setScroll(this.#clip.scrollTop);
    }
    this.#paint();
    if (motion) this.#play(motion);
    if (reveal) {
      // Along its lane too, if the selection is in one and scrolled out of it.
      const along = this.#core.revealLane();
      const lane = along && this.#lanes.get(along.lane);
      if (lane) this.#scrollLane(lane, along.x);
    }
    if (changed) this.#on.viewChanged?.();
  }

  /** Paints again what is in view, as pictures arrive. */
  repaint() {
    this.#painted = null;
    this.#paint();
  }

  // ---- The sheet ----

  // The style's sheet, as the engine draws it: a wash from top to bottom, a vignette, and a
  // drafting grid where the style has one.
  #drawSheet() {
    const s = this.#core.sheet(this.#style);
    this.#sheetSpec = s;
    const rgb = (c) => `rgb(${c.map((v) => Math.round(v * 255)).join(" ")})`;
    const layers = [];
    if (s.gridPitch > 0) {
      const line = `rgb(${s.grid.map((v) => Math.round(v * 255)).join(" ")} / ${s.gridAlpha})`;
      const major = `rgb(${s.grid.map((v) => Math.round(v * 255)).join(" ")} / ${s.gridAlpha * 1.6})`;
      const p = s.gridPitch;
      layers.push(`repeating-linear-gradient(0deg, ${major} 0 1px, transparent 1px ${p * 5}px)`,
                  `repeating-linear-gradient(90deg, ${major} 0 1px, transparent 1px ${p * 5}px)`,
                  `repeating-linear-gradient(0deg, ${line} 0 1px, transparent 1px ${p}px)`,
                  `repeating-linear-gradient(90deg, ${line} 0 1px, transparent 1px ${p}px)`);
    }
    if (s.vignette > 0) layers.push(`radial-gradient(circle at 50% 50%, transparent 30%, rgb(0 0 0 / ${s.vignette}) 100%)`);
    layers.push(`linear-gradient(${rgb(s.top)}, ${rgb(s.bottom)})`);
    this.#sheetBackground = layers.join(", ");
    this.#sheet.style.background = this.#sheetBackground;
    this.element.dataset.dark = s.dark ? "true" : "false";
  }

  // ---- Painting ----

  #paint() {
    const { height } = this.contentArea;
    const top = this.#clip.scrollTop, bottom = top + height;
    this.#shownTop = top;
    this.#placeSticky(top);
    if (this.#painted && this.#painted.top <= top && this.#painted.bottom >= bottom) return;
    const window = { top: top - height, bottom: bottom + height };
    this.#painted = window;
    for (const layer of Object.values(this.#layers)) layer.used = { rect: 0, text: 0, picture: 0, line: 0 };
    this.#turnBox.hidden = true; // shown again below, if Peek still turns
    for (const layer of this.#laneLayers.values()) layer.used = { rect: 0, text: 0, picture: 0, line: 0 };
    this.#frames.clear();
    this.#sticky = [];
    const items = this.#core.items;
    if (this.#lanesOf !== items) {
      this.#lanes = new Map(this.#core.lanes.map((l) => [l.id, l]));
      this.#lanesOf = items;
    }
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      const fixed = (item.flags & Flags.Fixed) !== 0;
      const sticky = !fixed && (item.stickyTop !== 0 || item.stickyUntil !== 0);
      if (!fixed) {
        // A heading that stays at the top is painted wherever its block shows.
        const low = Math.min(item.y, item.y + item.height);
        const high = Math.max(item.y, item.y + item.height) + (sticky ? item.stickyUntil : 0);
        if (high < window.top - 8 || low > window.bottom + 8) continue;
      }
      let layer = fixed ? this.#layers.fixed : this.#layers.content;
      if (item.lane) {
        // In a lane: painted a lane's width either side of what shows.
        const lane = this.#lanes.get(item.lane);
        if (!lane) continue;
        const scroll = this.#laneScroll.get(item.lane) ?? 0;
        const [lx, , lw] = lane.rect;
        const left = Math.min(item.x, item.x + item.width), right = Math.max(item.x, item.x + item.width);
        if (right < lx + scroll - lw || left > lx + scroll + 2 * lw) continue;
        layer = this.#laneLayer(lane, i);
      }
      let node;
      switch (item.kind) {
        case Kinds.Rect:
          node = this.#reuse(layer, "rect", "g-rect");
          this.#paintRect(node, item);
          break;
        case Kinds.Text:
          node = this.#reuse(layer, "text", "g-text");
          this.#paintText(node, item);
          break;
        case Kinds.Picture:
          if (item.flags & Flags.Turn && this.#turntable?.file === item.file) {
            // Peek's turntable: its own box, over the picture area, the frame shown drawn in it.
            node = this.#turnBox;
            node.hidden = false;
            setBox(node, item.x, item.y, item.width, item.height);
            this.#drawTurn(item.width, item.height);
            break;
          }
          node = this.#reuse(layer, "picture", "g-picture", "img");
          // Drawn large: its detail picture, framed as the picture, once it has come.
          node.src = (item.flags & Flags.Detail ? this.#details.get(item.file) : null) ?? this.#images.get(item.file) ?? "";
          node.style.visibility = this.#images.has(item.file) ? "" : "hidden";
          node.dataset.file = String(item.file);
          setBox(node, item.x, item.y, item.width, item.height);
          break;
        case Kinds.Line:
          node = this.#reuse(layer, "line", "g-line");
          this.#paintLine(node, item);
          break;
        default:
          continue;
      }
      const base = item.kind === Kinds.Rect && item.flags & Flags.Hatched ? 0.8 : 1;
      const opacity = base * (item.flags & Flags.Fading ? kFaded : 1) * (item.flags & Flags.Stale ? kStale : 1) *
        (item.flags & Flags.Lost ? kLost : 1);
      node.style.opacity = opacity === 1 ? "" : String(opacity);
      // Headings stay above what scrolls under them.
      node.style.zIndex = String(sticky ? items.length + i : i);
      node.style.transform = node.dataset.turn ?? "";
      if (sticky) this.#sticky.push({ node, item, index: i });
      this.#frames.set(i, node);
    }
    for (const layer of [...Object.values(this.#layers), ...this.#laneLayers.values()]) {
      for (const kind of Object.keys(layer.pools)) {
        const pool = layer.pools[kind];
        for (let i = layer.used[kind]; i < pool.length; i++) pool[i].hidden = true;
      }
    }
    for (const [id, layer] of this.#laneLayers) {
      layer.box.hidden = !this.#lanes.has(id) || Object.values(layer.used).every((n) => n === 0);
    }
    this.#placeSticky(top);
  }

  // A lane's layer: a box clipped to the lane, holding a layer placed in content points and moved
  // by the lane's scroll. Stacked where the lane's first item is.
  #laneLayer(lane, index) {
    let layer = this.#laneLayers.get(lane.id);
    if (!layer) {
      const box = el("div", "g-lane");
      const node = el("div", "g-lane-content");
      box.append(node);
      this.#content.append(box);
      layer = { box, node, pools: { rect: [], text: [], picture: [], line: [] }, used: null, z: -1 };
      layer.used = { rect: 0, text: 0, picture: 0, line: 0 };
      this.#laneLayers.set(lane.id, layer);
    }
    if (layer.z === -1 || Object.values(layer.used).every((n) => n === 0)) {
      const [x, y, w, h] = lane.rect;
      setBox(layer.box, x, y, w, h);
      layer.box.style.zIndex = String(index);
      layer.node.style.left = `${-x}px`;
      layer.node.style.top = `${-y}px`;
      layer.node.style.transform = `translateX(${-(this.#laneScroll.get(lane.id) ?? 0)}px)`;
      layer.z = index;
    }
    return layer;
  }

  #laneAt({ x, y }) {
    for (const lane of this.#lanes.values()) {
      const [lx, ly, lw, lh] = lane.rect;
      if (x >= lx && x <= lx + lw && y >= ly && y <= ly + lh) return lane;
    }
    return null;
  }

  // Scrolls a lane sideways, within its content, painting further along it as it goes.
  #scrollLane(lane, x) {
    const most = Math.max(0, lane.contentWidth - lane.rect[2]);
    const to = Math.min(Math.max(0, x), most);
    if (to === (this.#laneScroll.get(lane.id) ?? 0)) return;
    this.#laneScroll.set(lane.id, to);
    this.#core.setLaneScroll(lane.id, to);
    const layer = this.#laneLayers.get(lane.id);
    if (layer) layer.node.style.transform = `translateX(${-to}px)`;
    this.#painted = null;
    this.reload();
    this.#on.viewChanged?.();
  }

  // Moves each heading down by how far the view has scrolled past it, until the next takes over.
  #placeSticky(top) {
    this.#lifted.clear();
    for (const { node, item, index } of this.#sticky) {
      const dy = Math.min(Math.max(top - item.stickyTop, 0), item.stickyUntil);
      node.style.transform = dy > 0 ? `translateY(${dy}px)` : "";
      if (dy > 0) this.#lifted.set(index, dy);
    }
  }

  #reuse(layer, kind, className, tag = "div") {
    const pool = layer.pools[kind];
    if (layer.used[kind] === pool.length) {
      const node = el(tag, className);
      if (tag === "img") {
        node.alt = "";
        node.draggable = false;
        node.decoding = "async";
      }
      layer.node.append(node);
      pool.push(node);
    }
    const node = pool[layer.used[kind]++];
    node.hidden = false;
    return node;
  }

  #paintRect(node, item) {
    setBox(node, item.x, item.y, item.width, item.height);
    const s = node.style;
    s.borderRadius = `${item.radius}px`;
    // A box no deeper than its two strokes is all stroke, as a layer draws it. A border would grow
    // it (Chrome rounds a border up to a whole pixel), so it is filled with the stroke instead.
    const allStroke = item.stroke && 2 * item.lineWidth >= Math.min(Math.abs(item.width), Math.abs(item.height));
    s.border = item.stroke && !allStroke ? `${item.lineWidth}px ${item.flags & Flags.Dashed ? "dashed" : "solid"} ${cssColour(item.stroke)}` : "none";
    s.boxShadow = item.flags & Flags.Sticker && item.shadow ? `3px 3px 0 ${cssColour(item.shadow)}` : "none";
    if (item.flags & Flags.Ruler) {
      // A ruler: its line on top, minor ticks every `radius` points, major every `tracking`.
      const c = cssColour(item.stroke), faint = `color-mix(in srgb, ${c} 50%, transparent)`;
      s.border = "none";
      s.borderTop = `${item.lineWidth}px solid ${c}`;
      s.borderRadius = "0";
      s.boxShadow = "none";
      s.background = `repeating-linear-gradient(to right, ${c} 0 1.1px, transparent 1.1px ${item.tracking}px) 0 0 / 100% 6.5px no-repeat, ` +
        `repeating-linear-gradient(to right, ${faint} 0 0.75px, transparent 0.75px ${item.radius}px) 0 0 / 100% 3.5px no-repeat`;
    } else if (item.flags & Flags.Hatched) {
      s.background = `repeating-linear-gradient(45deg, ${cssColour(item.stroke)} 0 1px, transparent 1px 7px)`;
    } else if (item.flags & Flags.Sheet) {
      s.background = this.#sheetBackground;
    } else if (allStroke) {
      s.background = cssColour(item.stroke);
    } else {
      s.background = item.fill ? cssColour(item.fill) : "transparent";
    }
  }

  // Where the browser sets a line's baseline below its top, for a size and weight: measured once,
  // since browsers round a font's ascent to the pixel at some sizes, where the core reckons 1.025 em.
  #ascents = new Map();
  #ascent(size, weight) {
    const key = `${size}/${weight}`;
    let ascent = this.#ascents.get(key);
    if (ascent === undefined) {
      const line = el("div", "g-text");
      Object.assign(line.style, { position: "absolute", visibility: "hidden", left: "0", top: "0",
                                  fontSize: `${size}px`, fontWeight: String(weight), height: `${kLine * size}px` });
      const probe = el("span", "g-probe");
      line.append("M", probe);
      this.#content.append(line);
      ascent = probe.getBoundingClientRect().bottom - line.getBoundingClientRect().top;
      line.remove();
      if (!(ascent > 0)) ascent = kAscent * size;
      // Kept only once the font is in, or it would keep a fallback's.
      if (document.fonts?.status === "loaded") this.#ascents.set(key, ascent);
    }
    return ascent;
  }

  #paintText(node, item) {
    const size = item.fontSize;
    // The line's box starts its ascent above the baseline, as this browser sets it.
    setBox(node, item.x, item.baseline - this.#ascent(size, item.weight), item.width + 1, kLine * size);
    const s = node.style;
    s.fontSize = `${size}px`;
    s.fontWeight = String(item.weight);
    s.letterSpacing = `${item.tracking}px`;
    s.color = cssColour(item.fill);
    s.textAlign = item.align === 1 ? "center" : item.align === 2 ? "right" : "left";
    const text = this.#core.text(item);
    if (this.#probes) {
      // A mark on the baseline, which the checks measure as the browser set the text.
      node.replaceChildren(document.createTextNode(text), el("span", "g-probe"));
    } else if (node.textContent !== text || node.childNodes.length !== 1) {
      node.textContent = text;
    }
  }

  #paintLine(node, item) {
    const horizontal = Math.abs(item.height) < Math.abs(item.width);
    const w = item.lineWidth;
    const style = item.flags & Flags.Dashed ? "dashed" : "solid";
    node.dataset.turn = "";
    if (Math.abs(item.width) > 0.5 && Math.abs(item.height) > 0.5) {
      // A slanted line, a detail's leader: a level one as long, turned about its start.
      const length = Math.hypot(item.width, item.height);
      setBox(node, item.x, item.y - w / 2, length, 0);
      node.style.borderTop = `${w}px ${style} ${cssColour(item.stroke)}`;
      node.style.borderLeft = "none";
      node.style.transformOrigin = `0 ${w / 2}px`;
      node.dataset.turn = `rotate(${Math.atan2(item.height, item.width)}rad)`;
      return;
    }
    if (horizontal) {
      setBox(node, Math.min(item.x, item.x + item.width), item.y - w / 2, Math.abs(item.width), 0);
      node.style.borderTop = `${w}px ${style} ${cssColour(item.stroke)}`;
      node.style.borderLeft = "none";
    } else {
      setBox(node, item.x - w / 2, Math.min(item.y, item.y + item.height), 0, Math.abs(item.height));
      node.style.borderLeft = `${w}px ${style} ${cssColour(item.stroke)}`;
      node.style.borderTop = "none";
    }
  }

  // ---- The poster's picture ----

  /**
   * The poster as a PNG, `scale` pixels a point: the window's width, and as tall as it is or as the
   * poster runs. Drawn on a canvas from the display list, as the elements paint it.
   */
  async posterPNG(scale = 2) {
    const { canvas } = await this.posterCanvas(scale);
    return new Promise((resolve) => canvas.toBlob(resolve, "image/png"));
  }

  /** The poster as a JPEG for a PDF's page: its bytes, pixels, and size in points. */
  async posterJPEG(scale = 2) {
    const { canvas, width, height } = await this.posterCanvas(scale);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.92));
    // A PDF's page is 14,400 points a side at most: a taller poster's page is smaller, whole.
    const k = Math.min(1, 14400 / Math.max(width, height));
    return { jpeg: new Uint8Array(await blob.arrayBuffer()), width: canvas.width, height: canvas.height,
             points: [width * k, height * k] };
  }

  /**
   * The poster drawn on a canvas, `scale` pixels a point or fewer, and its size in points. A very
   * tall poster is drawn smaller: a canvas holds 32,767 pixels a side at most, and 60 million
   * pixels is plenty.
   */
  async posterCanvas(scale = 2) {
    const host = this.element.getBoundingClientRect();
    const area = this.#stage.getBoundingClientRect();
    const left = area.left - host.left, top = area.top - host.top;
    const bottom = host.bottom - area.bottom;
    const width = host.width;
    const height = Math.max(host.height, top + this.#core.contentHeight + bottom);
    scale = Math.min(scale, 32000 / height, Math.sqrt(60e6 / (width * height)));
    const canvas = document.createElement("canvas");
    canvas.width = Math.round(width * scale);
    canvas.height = Math.round(height * scale);
    const g = canvas.getContext("2d");
    g.scale(scale, scale);
    this.#paintSheetOn(g, width, height);
    await document.fonts?.ready;
    const items = this.#core.items;
    // Each picture once, as a bitmap.
    const bitmaps = new Map();
    await Promise.all([...new Set(items.filter((i) => i.kind === Kinds.Picture).map((i) => i.file))].map(async (file) => {
      const url = this.#images.get(file);
      if (!url) return;
      try {
        bitmaps.set(file, await createImageBitmap(await (await fetch(url)).blob()));
      } catch {
        // A picture that cannot be read is left out, as the page leaves it hidden.
      }
    }));
    g.translate(left, top);
    const lanes = new Map(this.#core.lanes.map((l) => [l.id, l]));
    for (const item of items) {
      if (item.flags & Flags.Fixed) continue;
      // An item in a lane: clipped to the lane, moved by its scroll.
      const lane = item.lane ? lanes.get(item.lane) : null;
      if (lane) {
        g.save();
        g.beginPath();
        g.rect(...lane.rect);
        g.clip();
        g.translate(-(this.#laneScroll.get(item.lane) ?? 0), 0);
      }
      this.#canvasItem(g, item, bitmaps, width, height);
      if (lane) g.restore();
    }
    for (const bitmap of bitmaps.values()) bitmap.close();
    return { canvas, width, height };
  }

  #canvasItem(g, item, bitmaps, width, height) {
    {
      const base = item.kind === Kinds.Rect && item.flags & Flags.Hatched ? 0.8 : 1;
      g.globalAlpha = base * (item.flags & Flags.Fading ? kFaded : 1) * (item.flags & Flags.Stale ? kStale : 1) *
        (item.flags & Flags.Lost ? kLost : 1);
      switch (item.kind) {
        case Kinds.Rect: this.#canvasRect(g, item, width, height); break;
        case Kinds.Text: this.#canvasText(g, item); break;
        case Kinds.Picture: {
          const bitmap = bitmaps.get(item.file);
          if (bitmap) g.drawImage(bitmap, item.x, item.y, item.width, item.height);
          break;
        }
        case Kinds.Line:
          g.beginPath();
          g.moveTo(item.x, item.y);
          g.lineTo(item.x + item.width, item.y + item.height);
          g.strokeStyle = cssColour(item.stroke);
          g.lineWidth = item.lineWidth;
          g.setLineDash(item.flags & Flags.Dashed ? [4, 3] : []);
          g.stroke();
          break;
      }
    }
  }

  // The style's sheet, as #drawSheet paints it with gradients.
  #paintSheetOn(g, width, height) {
    const s = this.#sheetSpec ?? this.#core.sheet(this.#style);
    const rgb = (c, a = 1) => `rgb(${c.map((v) => Math.round(v * 255)).join(" ")} / ${a})`;
    const wash = g.createLinearGradient(0, 0, 0, height);
    wash.addColorStop(0, rgb(s.top));
    wash.addColorStop(1, rgb(s.bottom));
    g.fillStyle = wash;
    g.fillRect(0, 0, width, height);
    if (s.vignette > 0) {
      const r = Math.hypot(width, height) / 2;
      const dark = g.createRadialGradient(width / 2, height / 2, 0, width / 2, height / 2, r);
      dark.addColorStop(0.3, "rgb(0 0 0 / 0)");
      dark.addColorStop(1, `rgb(0 0 0 / ${s.vignette})`);
      g.fillStyle = dark;
      g.fillRect(0, 0, width, height);
    }
    if (s.gridPitch > 0) {
      for (const [step, alpha] of [[s.gridPitch, s.gridAlpha], [s.gridPitch * 5, s.gridAlpha * 1.6]]) {
        g.fillStyle = rgb(s.grid, alpha);
        for (let x = 0; x < width; x += step) g.fillRect(x, 0, 1, height);
        for (let y = 0; y < height; y += step) g.fillRect(0, y, width, 1);
      }
    }
  }

  #canvasRect(g, item, width, height) {
    if (item.flags & Flags.Ruler) {
      // Its line, then minor and major ticks below it.
      g.fillStyle = cssColour(item.stroke);
      g.fillRect(item.x, item.y, item.width, item.lineWidth);
      for (const [pitch, w, h, alpha] of [[item.radius, 0.75, 3.5, 0.5], [item.tracking, 1.1, 6.5, 1]]) {
        if (!(pitch > 0.5)) continue;
        g.globalAlpha *= alpha;
        for (let x = 0; x < item.width; x += pitch) g.fillRect(item.x + x, item.y + 1, w, h);
        g.globalAlpha /= alpha;
      }
      return;
    }
    const path = () => {
      g.beginPath();
      g.roundRect(item.x, item.y, item.width, item.height, item.radius);
    };
    if (item.flags & Flags.Sticker && item.shadow) {
      g.beginPath();
      g.roundRect(item.x + 3, item.y + 3, item.width, item.height, item.radius);
      g.fillStyle = cssColour(item.shadow);
      g.fill();
    }
    path();
    if (item.flags & Flags.Hatched) {
      g.save();
      g.clip();
      g.strokeStyle = cssColour(item.stroke);
      g.lineWidth = 1;
      g.beginPath();
      for (let d = -item.height; d < item.width; d += 7) {
        g.moveTo(item.x + d, item.y + item.height);
        g.lineTo(item.x + d + item.height, item.y);
      }
      g.stroke();
      g.restore();
      path();
    } else if (item.flags & Flags.Sheet) {
      g.save();
      g.clip();
      this.#paintSheetOn(g, width, height);
      g.restore();
      path();
    } else if (item.fill) {
      g.fillStyle = cssColour(item.fill);
      g.fill();
    }
    if (item.stroke) {
      g.strokeStyle = cssColour(item.stroke);
      g.lineWidth = item.lineWidth;
      g.setLineDash(item.flags & Flags.Dashed ? [4, 3] : []);
      g.stroke();
    }
  }

  #canvasText(g, item) {
    g.font = `${item.weight} ${item.fontSize}px "Plex Mono", ui-monospace, monospace`;
    g.letterSpacing = `${item.tracking}px`;
    g.fillStyle = cssColour(item.fill);
    g.textBaseline = "alphabetic";
    g.textAlign = item.align === 1 ? "center" : item.align === 2 ? "right" : "left";
    const x = item.align === 1 ? item.x + item.width / 2 : item.align === 2 ? item.x + item.width : item.x;
    g.fillText(this.#core.text(item), x, item.baseline);
  }

  // ---- Motion ----

  // A copy of the view as last painted, beneath the content, for the old view's part in a move.
  #copyOfShown() {
    this.#ghost?.remove();
    const ghost = el("div", "gallery-ghost");
    const inner = el("div", "gallery-ghost-inner");
    const content = this.#content.cloneNode(true);
    for (const node of content.querySelectorAll("[hidden]")) node.remove();
    content.style.transform = `translateY(${-this.#shownTop}px)`;
    inner.append(content);
    ghost.append(inner);
    this.#stage.insertBefore(ghost, this.#clip);
    this.#ghost = ghost;
    return ghost;
  }

  #play({ transition: t, tracks, ghost }) {
    const inner = ghost.firstChild;
    const done = () => {
      ghost.remove();
      if (this.#ghost === ghost) this.#ghost = null;
    };
    const { width: W, height: H } = this.contentArea;
    const [px, py, pw, ph] = t.rect;
    // The transform that puts the whole view into the plate, and the one that blows the plate up
    // to fill the view, keeping proportions.
    const into = () => {
      const s = Math.min(pw / W, ph / H);
      return `translate(${px + (pw - W * s) / 2}px, ${py + (ph - H * s) / 2}px) scale(${s})`;
    };
    const outOf = () => {
      const s = Math.min(W / pw, H / ph);
      return `translate(${(W - pw * s) / 2 - px * s}px, ${(H - ph * s) / 2 - py * s}px) scale(${s})`;
    };
    const timing = { duration: t.duration, easing: kEase };
    if (t.kind === Transitions.Into) {
      // The folder grows out of its plate; the plates around it rush past.
      this.#clip.animate([{ transformOrigin: "0 0", transform: into(), opacity: 0 }, { opacity: 1, offset: 0.4 },
                          { transformOrigin: "0 0", transform: "none", opacity: 1 }], timing);
      inner.animate([{ transformOrigin: "0 0", transform: "none" }, { transformOrigin: "0 0", transform: outOf() }],
                    timing).onfinish = done;
      return;
    }
    if (t.kind === Transitions.Out) {
      // The folder shrinks back into its plate; the plates around it settle.
      this.#clip.animate([{ transformOrigin: "0 0", transform: outOf(), opacity: 0.3 },
                          { transformOrigin: "0 0", transform: "none", opacity: 1 }], timing);
      inner.animate([{ transformOrigin: "0 0", transform: "none" }, { transformOrigin: "0 0", transform: into() }],
                    timing).onfinish = done;
      return;
    }
    // A switch: parts in both views fly from where they were, the rest fade in, the old view fades.
    const items = this.#core.items;
    const top = this.#clip.scrollTop;
    for (const track of tracks) {
      const node = this.#frames.get(track.item);
      const item = items[track.item];
      if (!node || !item) continue;
      if (track.kind === TrackKinds.Fly) {
        const [fx, fy, fw] = track.from;
        const dx = fx - item.x, dy = fy - (item.y - top), s = item.width > 0 ? fw / item.width : 1;
        node.animate([{ transformOrigin: "0 0", transform: `translate(${dx}px, ${dy}px) scale(${s})` },
                      { transformOrigin: "0 0", transform: "none" }],
                     { duration: track.duration, delay: track.delay, easing: kEase, fill: "backwards" });
        for (const old of inner.querySelectorAll(`img[data-file="${item.file}"]`)) old.style.visibility = "hidden";
      } else {
        const end = Number(node.style.opacity || 1);
        node.animate([{ opacity: 0 }, { opacity: end }],
                     { duration: track.duration, delay: track.delay, easing: "ease-out", fill: "backwards" });
      }
    }
    this.#clip.animate([{ opacity: 0.001 }, { opacity: 1 }], { duration: 160, easing: "ease-out" });
    inner.animate([{ opacity: 1 }, { opacity: 0 }], { duration: t.fadeOut, easing: "ease-out", fill: "forwards" })
      .onfinish = done;
  }

  // ---- Checks ----

  /**
   * Where each item's aligned line and left edge were painted, in content points, NaN where not
   * painted: a text's baseline from its probe, as the browser set it; a picture's part from its box.
   */
  measured() {
    const items = this.#core.items;
    const contentOrigin = this.#content.getBoundingClientRect();
    const fixedOrigin = this.#fixed.getBoundingClientRect();
    const lines = new Float32Array(items.length).fill(NaN);
    const columns = new Float32Array(items.length).fill(NaN);
    for (const [i, node] of this.#frames) {
      const item = items[i];
      // In the layout's points: fixed items from the view's corner, headings less how far they
      // were moved down to stay at the top.
      const origin = item.flags & Flags.Fixed ? fixedOrigin : contentOrigin;
      const lifted = this.#lifted.get(i) ?? 0;
      const box = node.getBoundingClientRect();
      const top = box.top - origin.top - lifted;
      const left = box.left - origin.left + (item.lane ? this.#laneScroll.get(item.lane) ?? 0 : 0);
      if (item.kind === Kinds.Text) {
        const probe = node.querySelector(".g-probe");
        lines[i] = probe ? probe.getBoundingClientRect().bottom - origin.top - lifted : top + kAscent * item.fontSize;
      } else if (item.kind === Kinds.Picture) {
        const fraction = item.height ? (item.line - item.y) / item.height : 1;
        lines[i] = top + fraction * box.height;
      } else if (item.kind === Kinds.Line) {
        // The box's top is above the line's by half the stroke, as much of it as runs upright: all
        // of it level, none of it upright, and in between as the line slants.
        const length = Math.hypot(item.width, item.height) || 1;
        lines[i] = top + (item.lineWidth / 2) * Math.abs(item.width) / length;
      } else if (item.flags & Flags.Ruler) {
        lines[i] = top + item.lineWidth / 2; // a ruler stands on its line, its top border
      } else {
        lines[i] = top + box.height;
      }
      const leftOf = item.kind === Kinds.Line && Math.abs(item.height) >= Math.abs(item.width) ? left + item.lineWidth / 2 : left;
      columns[i] = leftOf + (item.columnLine - item.x);
    }
    return { lines, columns };
  }

  /** Where each item was painted, in content points, for comparing with the Mac's. */
  paintedBoxes() {
    const items = this.#core.items;
    const contentOrigin = this.#content.getBoundingClientRect();
    const fixedOrigin = this.#fixed.getBoundingClientRect();
    const boxes = new Map();
    for (const [i, node] of this.#frames) {
      const origin = items[i].flags & Flags.Fixed ? fixedOrigin : contentOrigin;
      const box = node.getBoundingClientRect();
      const along = items[i].lane ? this.#laneScroll.get(items[i].lane) ?? 0 : 0;
      boxes.set(i, { x: box.left - origin.left + along, y: box.top - origin.top - (this.#lifted.get(i) ?? 0), w: box.width, h: box.height });
    }
    return boxes;
  }

  // ---- Input ----

  #point(e) {
    const box = this.#content.getBoundingClientRect();
    return { x: e.clientX - box.left, y: e.clientY - box.top };
  }

  #hover(e) {
    const { x, y } = this.#point(e);
    const hit = this.#core.hit(x, y);
    if (this.#core.hover(hit.kind, hit.target)) this.reload();
    const pointing = hit.kind === Targets.Folder || hit.kind === Targets.File || hit.kind === Targets.More ||
      hit.kind === Targets.Reconnect || hit.kind === Targets.Page || (hit.kind === Targets.Peek && hit.target === 1);
    this.#clip.style.cursor = this.#overTurntable(e) ? "ew-resize" : pointing ? "pointer" : "";
  }

  // The map moved: drawn again from the camera, the toolbar told of its level.
  #atlasMoved() {
    this.#painted = null;
    this.reload();
    this.#on.atlasMoved?.();
    this.#on.viewChanged?.();
  }

  #click(e, twice) {
    const { x, y } = this.#point(e);
    const hit = this.#core.hit(x, y);
    if (twice && hit.kind === Targets.None && this.#core.view === Views.Atlas) {
      this.#core.atlasZoom(2.2, x, y); // a double-click on the map goes a level in
      this.#atlasMoved();
      return;
    }
    this.act(this.#core.click(hit.kind, hit.target, twice));
  }

  // ---- Detail pictures ----

  /** A file's detail picture (a PNG blob), for where it is drawn large. */
  setDetail(file, png) {
    const old = this.#details.get(file);
    if (old) URL.revokeObjectURL(old);
    this.#details.set(file, URL.createObjectURL(png));
    this.repaint();
  }

  /** Detail pictures held, for the checks. */
  get detailCount() { return this.#details.size; }

  /** Lets go of the detail pictures of files no longer drawn large. */
  keepDetails(files) {
    for (const [file, url] of this.#details) {
      if (!files.has(file)) {
        URL.revokeObjectURL(url);
        this.#details.delete(file);
      }
    }
  }

  // ---- Peek's turntable ----

  /** The turntable's frames for the file in Peek, or null: its still picture shows meanwhile. */
  setTurntable(turntable) {
    for (const frame of this.#turntable?.frames ?? []) frame.close?.();
    this.#turntable = turntable;
    this.#turnFrame = 0;
    this.#painted = null;
  }

  /** The turntable's frame showing, for the checks. */
  get turnFrameShown() { return this.#turnFrame; }

  /** Shows a frame of the turntable, counted round. */
  #showTurnFrame(frame) {
    const count = this.#turntable?.frames.length ?? 0;
    if (!count) return;
    this.#turnFrame = ((frame % count) + count) % count;
    const box = this.#turnBox.getBoundingClientRect();
    this.#drawTurn(box.width, box.height);
  }

  // Draws the frame shown into the box `w` × `h`: every frame fitted alike, as the board fits them,
  // by the box holding all their ink and 12 pixels of room around it.
  #drawTurn(w, h) {
    const t = this.#turntable;
    const frame = t?.frames[this.#turnFrame];
    if (!frame) return;
    const ratio = devicePixelRatio || 1;
    const canvas = this.#turnCanvas;
    const cw = Math.round(w * ratio), ch = Math.round(h * ratio);
    if (canvas.width !== cw || canvas.height !== ch) {
      canvas.width = cw;
      canvas.height = ch;
    }
    const g = canvas.getContext("2d");
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    g.clearRect(0, 0, w, h);
    const [ix, iy, iw, ih] = t.ink;
    // As large as the area allows, but never past half a point a pixel: sharp on a 2× screen.
    const k = Math.min(w / (iw + 24), h / (ih + 24), 0.5);
    g.imageSmoothingQuality = "high";
    g.drawImage(frame, w / 2 - (ix + iw / 2) * k, h / 2 - (iy + ih / 2) * k, frame.width * k, frame.height * k);
  }

  #overTurntable(e) {
    if (this.#turnBox.hidden || !this.#turntable) return false;
    const box = this.#turnBox.getBoundingClientRect();
    return e.clientX >= box.left && e.clientX <= box.right && e.clientY >= box.top && e.clientY <= box.bottom;
  }

  /** Acts on what a click or key asked: lays out again, and tells the page. */
  act(action, { reveal = false } = {}) {
    if (action.kind === Actions.None) return;
    if (action.kind === Actions.Turn) this.#showTurnFrame(this.#turnFrame + (action.target === 0 ? -1 : 1));
    if (action.kind === Actions.EnterFolder) {
      this.scrollToTop();
      this.reload();
    } else if (action.kind === Actions.Handled || action.kind === Actions.Peek) {
      this.reload({ reveal });
    }
    this.#on.action?.(action);
  }

  /** A key: what it asks, and the selection scrolled into view. */
  key(key) {
    this.act(this.#core.key(key), { reveal: true });
  }

  /** Shows another view, if enabled, flying what both views show; the selection kept in view. */
  setView(view) {
    if (!this.#core.setView(view)) return false;
    this.reload({ reveal: true });
    return true;
  }
}

function el(tag, className) {
  const node = document.createElement(tag);
  node.className = className;
  return node;
}

function setBox(node, x, y, w, h) {
  const s = node.style;
  s.left = `${x}px`;
  s.top = `${y}px`;
  s.width = `${w}px`;
  s.height = `${h}px`;
}
