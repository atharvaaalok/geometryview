// Pointer, wheel, touch and key input on the viewport, turned into the engine's events as the Mac
// viewport turns AppKit's: drawable pixels from the top left, the same buttons and modifiers, the
// wheel's lines and pixels scaled as the Mac scales them, and pinches as magnification.

import { Buttons, Modifiers, Pointer, Tools } from "./engine.js";

// What each key does, as the Mac's default key map has it. `hold` keys pick a tool while down.
const kKeys = {
  h: { hold: Tools.Hide }, g: { hold: Tools.Ghost },
  f: { command: "fit" }, p: { command: "projection" }, Escape: { command: "clear" },
  i: { command: "isolate" }, H: { command: "showAll" }, G: { command: "unghostAll" },
  1: { command: "faces" }, 2: { command: "bodies" }, 3: { command: "parts" },
  s: { command: "section" }, t: { command: "seeThrough" }, "?": { command: "shortcuts" },
};

export class ViewportInput {
  #engine;
  #canvas;
  #onCommand;
  #onTool;
  #onPointer;
  #pressed = new Set(); // buttons this view sent a press for
  #touches = new Map(); // pointer id to its last position, for touch gestures
  #pinch = null;
  #space = false;
  #heldKey = null;

  // `onCommand(name)` for keys that are not tools, `onTool(tool)` when a held tool changes, and
  // `onPointer(x, y)` with the pointer's CSS position in the viewport, or null when it leaves.
  constructor(engine, canvas, { onCommand, onTool, onPointer }) {
    this.#engine = engine;
    this.#canvas = canvas;
    this.#onCommand = onCommand;
    this.#onTool = onTool;
    this.#onPointer = onPointer;
    canvas.addEventListener("pointerdown", (e) => this.#down(e));
    canvas.addEventListener("pointermove", (e) => this.#move(e));
    canvas.addEventListener("pointerup", (e) => this.#up(e));
    canvas.addEventListener("pointercancel", (e) => this.#cancel(e));
    canvas.addEventListener("pointerleave", (e) => this.#leave(e));
    canvas.addEventListener("wheel", (e) => this.#wheel(e), { passive: false });
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    // Safari's trackpad pinch.
    canvas.addEventListener("gesturestart", (e) => { e.preventDefault(); this.#gestureScale = 1; });
    canvas.addEventListener("gesturechange", (e) => this.#gesture(e));
    addEventListener("keydown", (e) => this.#keyDown(e));
    addEventListener("keyup", (e) => this.#keyUp(e));
    addEventListener("blur", () => this.releaseHeldTool());
  }

  #gestureScale = 1;

  #point(e) {
    const rect = this.#canvas.getBoundingClientRect();
    const scale = this.#engine.scale;
    return { x: (e.clientX - rect.left) * scale, y: (e.clientY - rect.top) * scale,
             cssX: e.clientX - rect.left, cssY: e.clientY - rect.top };
  }

  #modifiers(e) {
    return (e.shiftKey ? Modifiers.Shift : 0) | (e.ctrlKey ? Modifiers.Control : 0) |
           (e.altKey ? Modifiers.Option : 0) | (e.metaKey ? Modifiers.Command : 0) |
           (this.#space ? Modifiers.Space : 0);
  }

  #button(e) {
    return e.button === 0 ? Buttons.Left : e.button === 1 ? Buttons.Middle
         : e.button === 2 ? Buttons.Right : Buttons.None;
  }

  #post(kind, button, e, delta = 0) {
    const { x, y } = this.#point(e);
    this.#engine.pointer(kind, button, x, y, delta, this.#modifiers(e), e.timeStamp / 1000);
  }

  #down(e) {
    if (e.pointerType === "touch") {
      this.#touchDown(e);
      return;
    }
    const button = this.#button(e);
    if (button === Buttons.None) return;
    this.#canvas.setPointerCapture(e.pointerId);
    this.#pressed.add(button);
    this.#post(Pointer.Down, button, e);
  }

  #move(e) {
    if (e.pointerType === "touch") {
      this.#touchMove(e);
      return;
    }
    const { cssX, cssY } = this.#point(e);
    this.#onPointer?.(cssX, cssY);
    // Coalesced moves since the last frame, so a fast drag keeps its path.
    const events = e.getCoalescedEvents?.() ?? [e];
    for (const each of events.length ? events : [e]) this.#post(Pointer.Move, Buttons.None, each);
  }

  #up(e) {
    if (e.pointerType === "touch") {
      this.#touchUp(e);
      return;
    }
    const button = this.#button(e);
    if (!this.#pressed.delete(button)) return;
    this.#post(Pointer.Up, button, e);
  }

  #cancel(e) {
    if (e.pointerType === "touch") {
      this.#touchUp(e, true);
      return;
    }
    if (this.#pressed.size === 0) return;
    this.#pressed.clear();
    this.#post(Pointer.Cancel, Buttons.None, e);
  }

  #leave(e) {
    if (e.pointerType === "touch" || this.#pressed.size > 0) return;
    this.#onPointer?.(null);
    this.#post(Pointer.Leave, Buttons.None, e);
  }

  #wheel(e) {
    e.preventDefault();
    if (e.ctrlKey) {
      // A trackpad pinch arrives as a wheel with Control held, in Chrome and Firefox.
      this.#post(Pointer.Magnify, Buttons.None, e, -e.deltaY * 4);
      return;
    }
    // As the Mac scales its scroll deltas: precise pixels by 2, whole lines by 30. The browser's
    // count down where the Mac's count up, and a mouse wheel's notch arrives as about 100 pixels,
    // which is held to two lines' worth.
    const delta = e.deltaMode === 0 ? Math.max(-60, Math.min(60, -e.deltaY * 2))
                                    : -e.deltaY * 30;
    this.#post(Pointer.Scroll, Buttons.None, e, delta);
  }

  #gesture(e) {
    e.preventDefault();
    const change = e.scale / this.#gestureScale - 1;
    this.#gestureScale = e.scale;
    this.#post(Pointer.Magnify, Buttons.None, e, change * 400);
  }

  // ---- Touch: one finger rotates, two pan and pinch -----------------------------------------

  #touchDown(e) {
    this.#canvas.setPointerCapture(e.pointerId);
    this.#touches.set(e.pointerId, this.#point(e));
    if (this.#touches.size === 1) {
      this.#pressed.add(Buttons.Left);
      this.#post(Pointer.Down, Buttons.Left, e);
    } else if (this.#touches.size === 2) {
      // The rotation stops where it was, and a pan starts from between the fingers.
      this.#pressed.delete(Buttons.Left);
      this.#post(Pointer.Up, Buttons.Left, e);
      const centre = this.#centre();
      this.#pinch = { distance: this.#spread(), centre };
      this.#engine.pointer(Pointer.Down, Buttons.Middle, centre.x, centre.y, 0, 0, e.timeStamp / 1000);
      this.#pressed.add(Buttons.Middle);
    }
  }

  #touchMove(e) {
    if (!this.#touches.has(e.pointerId)) return;
    this.#touches.set(e.pointerId, this.#point(e));
    if (this.#touches.size === 1) {
      this.#post(Pointer.Move, Buttons.None, e);
    } else if (this.#touches.size === 2 && this.#pinch) {
      const centre = this.#centre();
      const spread = this.#spread();
      const time = e.timeStamp / 1000;
      this.#engine.pointer(Pointer.Move, Buttons.None, centre.x, centre.y, 0, 0, time);
      if (this.#pinch.distance > 0) {
        const change = spread / this.#pinch.distance - 1;
        this.#engine.pointer(Pointer.Magnify, Buttons.None, centre.x, centre.y, change * 400, 0, time);
      }
      this.#pinch = { distance: spread, centre };
    }
  }

  #touchUp(e, cancelled = false) {
    if (!this.#touches.has(e.pointerId)) return;
    const time = e.timeStamp / 1000;
    if (this.#touches.size === 2 && this.#pinch) {
      const { x, y } = this.#pinch.centre;
      this.#engine.pointer(Pointer.Up, Buttons.Middle, x, y, 0, 0, time);
      this.#pressed.delete(Buttons.Middle);
      this.#pinch = null;
    } else if (this.#touches.size === 1 && this.#pressed.delete(Buttons.Left)) {
      this.#post(cancelled ? Pointer.Cancel : Pointer.Up, Buttons.Left, e);
    }
    this.#touches.delete(e.pointerId);
    if (this.#touches.size === 0) {
      this.#engine.pointer(Pointer.Leave, Buttons.None, 0, 0, 0, 0, time);
    }
  }

  #centre() {
    const [a, b] = [...this.#touches.values()];
    return { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
  }

  #spread() {
    const [a, b] = [...this.#touches.values()];
    return Math.hypot(a.x - b.x, a.y - b.y);
  }

  // ---- Keys --------------------------------------------------------------------------------

  #keyDown(e) {
    if (e.target instanceof HTMLInputElement) return;
    if (e.code === "Space") {
      this.#space = true;
      e.preventDefault();
      return;
    }
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "z") {
      e.preventDefault();
      this.#onCommand?.(e.shiftKey ? "redo" : "undo");
      return;
    }
    if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    const binding = kKeys[e.key];
    if (!binding) return;
    e.preventDefault();
    if (binding.hold !== undefined) {
      this.#heldKey = e.code;
      this.#engine.setTool(binding.hold);
      this.#onTool?.(binding.hold);
    } else {
      this.#onCommand?.(binding.command);
    }
  }

  #keyUp(e) {
    if (e.code === "Space") {
      this.#space = false;
    } else if (e.code === this.#heldKey) {
      this.releaseHeldTool();
    }
  }

  // Drops a held tool, as when the page loses focus while its key is down.
  releaseHeldTool() {
    this.#space = false;
    if (this.#heldKey === null) return;
    this.#heldKey = null;
    this.#engine.setTool(Tools.Select);
    this.#onTool?.(Tools.Select);
  }
}
