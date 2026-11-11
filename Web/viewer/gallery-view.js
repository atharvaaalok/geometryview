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

import { Actions, cssColour, Flags, Kinds, Targets, TrackKinds, Transitions } from "./gallery-core.js";

const kAscent = 1.025, kLine = 1.3; // IBM Plex Mono, in ems, as the core reckons
const kFaded = 0.22; // what a search did not find
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
  #images = new Map(); // file -> object URL
  #painted = null; // { top, bottom } of what is painted
  #shownTop = 0; // the scroll last painted, where a copy of the old view is taken from
  #layers;
  #frames = new Map(); // item index -> element painted for it
  #sticky = []; // { node, item } of headings that stay at the top
  #lifted = new Map(); // item index -> how far a heading is moved down
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
    this.#clip.addEventListener("click", (e) => this.#click(e, e.detail >= 2));
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
    for (const url of this.#images.values()) URL.revokeObjectURL(url);
    this.#images.clear();
  }

  scrollToTop() {
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
    this.#frames.clear();
    this.#sticky = [];
    const items = this.#core.items;
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
      const layer = fixed ? this.#layers.fixed : this.#layers.content;
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
          node = this.#reuse(layer, "picture", "g-picture", "img");
          node.src = this.#images.get(item.file) ?? "";
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
      const opacity = base * (item.flags & Flags.Fading ? kFaded : 1);
      node.style.opacity = opacity === 1 ? "" : String(opacity);
      // Headings stay above what scrolls under them.
      node.style.zIndex = String(sticky ? items.length + i : i);
      node.style.transform = "";
      if (sticky) this.#sticky.push({ node, item, index: i });
      this.#frames.set(i, node);
    }
    for (const layer of Object.values(this.#layers)) {
      for (const kind of Object.keys(layer.pools)) {
        const pool = layer.pools[kind];
        for (let i = layer.used[kind]; i < pool.length; i++) pool[i].hidden = true;
      }
    }
    this.#placeSticky(top);
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
    s.border = item.stroke ? `${item.lineWidth}px ${item.flags & Flags.Dashed ? "dashed" : "solid"} ${cssColour(item.stroke)}` : "none";
    s.boxShadow = item.flags & Flags.Sticker && item.shadow ? `3px 3px 0 ${cssColour(item.shadow)}` : "none";
    if (item.flags & Flags.Hatched) {
      s.background = `repeating-linear-gradient(45deg, ${cssColour(item.stroke)} 0 1px, transparent 1px 7px)`;
    } else if (item.flags & Flags.Sheet) {
      s.background = this.#sheetBackground;
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
      const top = box.top - origin.top - lifted, left = box.left - origin.left;
      if (item.kind === Kinds.Text) {
        const probe = node.querySelector(".g-probe");
        lines[i] = probe ? probe.getBoundingClientRect().bottom - origin.top - lifted : top + kAscent * item.fontSize;
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
    const items = this.#core.items;
    const contentOrigin = this.#content.getBoundingClientRect();
    const fixedOrigin = this.#fixed.getBoundingClientRect();
    const boxes = new Map();
    for (const [i, node] of this.#frames) {
      const origin = items[i].flags & Flags.Fixed ? fixedOrigin : contentOrigin;
      const box = node.getBoundingClientRect();
      boxes.set(i, { x: box.left - origin.left, y: box.top - origin.top - (this.#lifted.get(i) ?? 0), w: box.width, h: box.height });
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
      (hit.kind === Targets.Peek && hit.target === 1);
    this.#clip.style.cursor = pointing ? "pointer" : "";
  }

  #click(e, twice) {
    const { x, y } = this.#point(e);
    const hit = this.#core.hit(x, y);
    this.act(this.#core.click(hit.kind, hit.target, twice));
  }

  /** Acts on what a click or key asked: lays out again, and tells the page. */
  act(action, { reveal = false } = {}) {
    if (action.kind === Actions.None) return;
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
