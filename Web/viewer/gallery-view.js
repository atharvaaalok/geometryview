// Paints the gallery's display list (gallery-core.js) with the DOM, over the style's sheet, and turns
// the pointer into the gallery's actions: the browser's counterpart of App/Sources/Gallery/
// GalleryView.swift. Everything it draws was placed by the layout core, so the Mac draws the same.
//
// Items are absolutely positioned elements, reused from one paint to the next by kind, and only
// those within a screen of the view are painted; scrolling within that moves nothing.

import { Actions, cssColour, Flags, Kinds, Targets } from "./gallery-core.js";

const kAscent = 1.025, kLine = 1.3; // IBM Plex Mono, in ems, as the core reckons

/** Where the content shows in the window: right of the Library card, above the toolbar. */
export const kContentInsets = Object.freeze({ top: 96, left: 340, bottom: 84, right: 18 });

export class GalleryView {
  element;
  #core;
  #on;
  #sheet;
  #clip;
  #content;
  #images = new Map(); // file -> object URL
  #painted = null; // { top, bottom } of what is painted
  #pools = { rect: [], text: [], picture: [], line: [] };
  #frames = new Map(); // item index -> element painted for it
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
    this.#clip = el("div", "gallery-clip");
    this.#content = el("div", "gallery-content");
    this.#clip.append(this.#content);
    this.element.append(this.#sheet, this.#clip);
    this.#clip.addEventListener("scroll", () => {
      this.#core.setScroll(this.#clip.scrollTop);
      this.#paint();
      this.#on.viewChanged?.();
    }, { passive: true });
    this.#clip.addEventListener("pointermove", (e) => this.#hover(e));
    this.#clip.addEventListener("pointerleave", () => {
      if (this.#core.hover(Targets.None, 0)) this.reload();
    });
    this.#clip.addEventListener("click", (e) => this.#click(e, e.detail >= 2));
    this.#place();
  }

  /** Where the content shows, in the window. */
  setInsets(insets) {
    this.#insets = insets;
    this.#place();
  }

  #place() {
    Object.assign(this.#clip.style, { left: `${this.#insets.left}px`, top: `${this.#insets.top}px`,
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
    for (const url of this.#images.values()) URL.revokeObjectURL(url);
    this.#images.clear();
  }

  scrollToTop() {
    this.#clip.scrollTop = 0;
    this.#core.setScroll(0);
    this.#painted = null;
  }

  /** Lays out again if anything changed, and paints what is in view. */
  reload() {
    const { width, height } = this.contentArea;
    if (width === 0 || height === 0) return;
    this.#core.setViewport(width, height);
    const changed = this.#core.layout();
    if (changed) this.#painted = null;
    this.#content.style.height = `${Math.max(this.#core.contentHeight, height)}px`;
    this.#paint();
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
    this.#sheet.style.background = layers.join(", ");
    this.element.dataset.dark = s.dark ? "true" : "false";
  }

  // ---- Painting ----

  #paint() {
    const { height } = this.contentArea;
    const top = this.#clip.scrollTop, bottom = top + height;
    if (this.#painted && this.#painted.top <= top && this.#painted.bottom >= bottom) return;
    const window = { top: top - height, bottom: bottom + height };
    this.#painted = window;
    const used = { rect: 0, text: 0, picture: 0, line: 0 };
    this.#frames.clear();
    const items = this.#core.items;
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      const low = Math.min(item.y, item.y + item.height), high = Math.max(item.y, item.y + item.height);
      if (high < window.top - 8 || low > window.bottom + 8) continue;
      let node;
      switch (item.kind) {
        case Kinds.Rect:
          node = this.#reuse("rect", used, "g-rect");
          this.#paintRect(node, item);
          break;
        case Kinds.Text:
          node = this.#reuse("text", used, "g-text");
          this.#paintText(node, item);
          break;
        case Kinds.Picture:
          node = this.#reuse("picture", used, "g-picture", "img");
          node.src = this.#images.get(item.file) ?? "";
          node.style.visibility = this.#images.has(item.file) ? "" : "hidden";
          setBox(node, item.x, item.y, item.width, item.height);
          break;
        case Kinds.Line:
          node = this.#reuse("line", used, "g-line");
          this.#paintLine(node, item);
          break;
        default:
          continue;
      }
      node.style.zIndex = String(i);
      this.#frames.set(i, node);
    }
    for (const kind of Object.keys(this.#pools)) {
      const pool = this.#pools[kind];
      for (let i = used[kind]; i < pool.length; i++) pool[i].hidden = true;
    }
  }

  #reuse(kind, used, className, tag = "div") {
    const pool = this.#pools[kind];
    if (used[kind] === pool.length) {
      const node = el(tag, className);
      if (tag === "img") {
        node.alt = "";
        node.draggable = false;
        node.decoding = "async";
      }
      this.#content.append(node);
      pool.push(node);
    }
    const node = pool[used[kind]++];
    node.hidden = false;
    return node;
  }

  #paintRect(node, item) {
    setBox(node, item.x, item.y, item.width, item.height);
    const s = node.style;
    s.borderRadius = `${item.radius}px`;
    s.border = item.stroke ? `${item.lineWidth}px ${item.flags & Flags.Dashed ? "dashed" : "solid"} ${cssColour(item.stroke)}` : "none";
    s.boxShadow = item.flags & Flags.Sticker && item.shadow ? `3px 3px 0 ${cssColour(item.shadow)}` : "none";
    if (item.flags & Flags.Hatched) {
      s.background = `repeating-linear-gradient(45deg, ${cssColour(item.stroke)} 0 1px, transparent 1px 7px)`;
      s.opacity = "0.8";
    } else {
      s.background = item.fill ? cssColour(item.fill) : "transparent";
      s.opacity = "";
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

  // ---- Checks ----

  /**
   * Where each item's aligned line and left edge were painted, in content points, NaN where not
   * painted: a text's baseline from its probe, as the browser set it; a picture's part from its box.
   */
  measured() {
    const items = this.#core.items;
    const origin = this.#content.getBoundingClientRect();
    const lines = new Float32Array(items.length).fill(NaN);
    const columns = new Float32Array(items.length).fill(NaN);
    for (const [i, node] of this.#frames) {
      const item = items[i];
      const box = node.getBoundingClientRect();
      const top = box.top - origin.top, left = box.left - origin.left;
      if (item.kind === Kinds.Text) {
        const probe = node.querySelector(".g-probe");
        lines[i] = probe ? probe.getBoundingClientRect().bottom - origin.top : top + kAscent * item.fontSize;
      } else if (item.kind === Kinds.Picture) {
        const fraction = item.height ? (item.line - item.y) / item.height : 1;
        lines[i] = top + fraction * box.height;
      } else if (item.kind === Kinds.Line) {
        const horizontal = Math.abs(item.height) < Math.abs(item.width);
        lines[i] = horizontal ? top + item.lineWidth / 2 : top;
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
    const origin = this.#content.getBoundingClientRect();
    const boxes = new Map();
    for (const [i, node] of this.#frames) {
      const box = node.getBoundingClientRect();
      boxes.set(i, { x: box.left - origin.left, y: box.top - origin.top, w: box.width, h: box.height });
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
    this.#clip.style.cursor = hit.kind !== Targets.None ? "pointer" : "";
  }

  #click(e, twice) {
    const { x, y } = this.#point(e);
    const hit = this.#core.hit(x, y);
    this.act(this.#core.click(hit.kind, hit.target, twice));
  }

  /** Acts on what a click or key asked: lays out again, and tells the page. */
  act(action) {
    if (action.kind === Actions.None) return;
    if (action.kind === Actions.EnterFolder) {
      this.scrollToTop();
      this.reload();
    } else if (action.kind === Actions.Handled) {
      this.reload();
    }
    this.#on.action?.(action);
  }

  /** Scrolls so a file's place shows. */
  key(key) {
    this.act(this.#core.key(key));
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
