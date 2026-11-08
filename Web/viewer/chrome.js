// The cards and overlays around the view, after the Mac app's Chrome folder: the masthead, the
// toolbar, the look, section and selection cards, the status card, the hold hint, the hover chip,
// the drop zone and the shortcuts sheet.

import { Actions, Tools } from "./engine.js";
import { icons } from "./icons.js";
import { el, formatNumber, keyCap, pill, segmented, slider } from "./ui.js";

// ---- The look ---------------------------------------------------------------------------------

export const Styles = [
  { title: "Illustration", usesColour: true, castsShadow: true, grain: true, wobble: true },
  { title: "Metal", usesColour: true, castsShadow: true, grain: false, wobble: false },
  { title: "Clay", usesColour: true, castsShadow: true, grain: false, wobble: false },
  { title: "Blueprint", usesColour: false, castsShadow: false, grain: true, wobble: false },
  { title: "Technical", usesColour: false, castsShadow: false, grain: false, wobble: false },
];
const Colours = [
  { title: "Material", summary: "Colours follow the material each name suggests." },
  { title: "Parts", summary: "Each part gets a colour of its own." },
  { title: "Mono", summary: "Every part in one neutral colour." },
  { title: "File", summary: "The colours stored in the file." },
];
const Ghosts = [{ title: "Shell", value: 1 }, { title: "Lines", value: 2 }, { title: "Both", value: 3 }];
const kWobbleSteps = [0, 1, 3, 5];

// How the model is drawn, as chosen; remembered in this browser between visits.
export class Look {
  style = 0; colour = 0; ghosts = 1; shadow = true; grain = true; wobble = 0;

  static saved() {
    const look = new Look();
    try {
      Object.assign(look, JSON.parse(localStorage.getItem("look") ?? "{}"));
    } catch {
      // A private window, or nothing saved yet.
    }
    return look;
  }

  save() {
    try {
      localStorage.setItem("look", JSON.stringify(this));
    } catch {
      // Not remembered, which costs nothing now.
    }
  }

  // Choosing a style also chooses its usual shadow.
  choose(style) {
    this.style = style;
    this.shadow = Styles[style].castsShadow;
  }

  get engineLook() {
    return { style: this.style, colorMode: this.colour, ghostStyle: this.ghosts, castShadow: this.shadow,
             grain: this.grain ? 1 : 0, wobble: this.wobble };
  }
}

export class LookCard {
  look;
  #rows = {};
  #note;
  #controls;

  constructor(look, onChange) {
    this.look = look;
    const edit = (change) => {
      const next = Object.assign(new Look(), this.look);
      change(next);
      this.show(next);
      onChange(next);
    };
    this.#controls = {
      style: segmented(Styles.map((s) => s.title), { help: "Drawing style", onChange: (i) => edit((l) => l.choose(i)) }),
      colour: segmented(Colours.map((c) => c.title), { help: "What decides each part's colour",
                                                       onChange: (i) => edit((l) => { l.colour = i; }) }),
      ghosts: segmented(Ghosts.map((g) => g.title), { help: "How ghosted parts look",
                                                      onChange: (i) => edit((l) => { l.ghosts = Ghosts[i].value; }) }),
      shadow: segmented(["On", "Off"], { help: "Cast shadow", onChange: (i) => edit((l) => { l.shadow = i === 0; }) }),
      grain: segmented(["On", "Off"], { help: "Paper grain", onChange: (i) => edit((l) => { l.grain = i === 0; }) }),
      wobble: segmented(["Off", "Light", "More", "Most"], { help: "Hand-drawn wobble",
                                                            onChange: (i) => edit((l) => { l.wobble = kWobbleSteps[i]; }) }),
    };
    this.#note = el("p.note");
    const row = (label, control) => {
      const text = el("span.label", { text: label });
      this.#rows[label] = { text, control };
      return el("div.row", {}, [text, control.element]);
    };
    this.element = el("section.card.popover.look", { hidden: true, "aria-label": "Look" }, [
      el("h2", { text: "LOOK" }),
      row("Style", this.#controls.style),
      row("Colour", this.#controls.colour),
      this.#note,
      row("Ghosts", this.#controls.ghosts),
      row("Shadow", this.#controls.shadow),
      row("Grain", this.#controls.grain),
      row("Hand-drawn", this.#controls.wobble),
    ]);
    this.show(look);
  }

  show(look) {
    this.look = look;
    const c = this.#controls;
    c.style.selected = look.style;
    c.colour.selected = look.colour;
    c.ghosts.selected = Math.max(0, Ghosts.findIndex((g) => g.value === look.ghosts));
    c.shadow.selected = look.shadow ? 0 : 1;
    c.grain.selected = look.grain ? 0 : 1;
    c.wobble.selected = Math.max(0, kWobbleSteps.indexOf(look.wobble));
    // What the style does not use is dimmed rather than hidden, so nothing moves under the pointer.
    const style = Styles[look.style];
    this.#enable("Colour", style.usesColour);
    this.#enable("Grain", style.grain);
    this.#enable("Hand-drawn", style.wobble);
    this.#note.textContent = style.usesColour ? Colours[look.colour].summary
                                              : `${style.title} draws every part alike.`;
  }

  #enable(label, enabled) {
    this.#rows[label].text.classList.toggle("disabled", !enabled);
    this.#rows[label].control.enabled = enabled;
  }
}

// ---- Section ----------------------------------------------------------------------------------

export class SectionCard {
  cuts = [0, 1, 2].map(() => ({ enabled: false, position: 0.5, flip: false }));
  #rows = [];

  constructor(onChange) {
    const edit = (axis, change) => {
      change(this.cuts[axis]);
      this.show();
      onChange(this.cuts);
    };
    const rows = ["X", "Y", "Z"].map((name, axis) => {
      const toggle = segmented(["Off", "Cut"], { help: `Cut across ${name}`,
                                                 onChange: (i) => edit(axis, (c) => { c.enabled = i === 1; }) });
      const position = slider({ help: `Where to cut across ${name}`,
                                onChange: (v) => edit(axis, (c) => { c.position = v; }) });
      const side = segmented(["+", "−"], { help: "Which side to remove",
                                           onChange: (i) => edit(axis, (c) => { c.flip = i === 1; }) });
      this.#rows.push({ toggle, position, side });
      return el("div.row", {}, [el("span.axis", { text: name }), toggle.element, position.element, side.element]);
    });
    this.element = el("section.card.popover.section", { hidden: true, "aria-label": "Section" },
                      [el("h2", { text: "SECTION" }), ...rows]);
    this.show();
  }

  get isCutting() { return this.cuts.some((c) => c.enabled); }

  show() {
    this.cuts.forEach((cut, axis) => {
      const row = this.#rows[axis];
      row.toggle.selected = cut.enabled ? 1 : 0;
      row.position.value = cut.position;
      row.position.enabled = cut.enabled;
      row.side.selected = cut.flip ? 1 : 0;
    });
  }
}

// ---- Selection --------------------------------------------------------------------------------

export class SelectionCard {
  #kind = el("div.kind");
  #name = el("div.name");
  #path = el("div.path");
  #faces = el("div.number");
  #bodies = el("div.number");

  constructor(onAction) {
    const stat = (number, unit) => el("div.stat", {}, [number, el("div.unit", { text: unit })]);
    const action = (title, help, a) => pill({ title, help, onPress: () => onAction(a) });
    this.element = el("section.card.selection", { hidden: true, "aria-label": "Selection" }, [
      el("div.head", {}, [this.#kind, this.#name, this.#path]),
      el("hr"),
      el("div.stats", {}, [stat(this.#faces, "faces"), stat(this.#bodies, "bodies")]),
      el("div.actions", {}, [
        action("Hide", "Hide the selection", Actions.HideSelection),
        action("Ghost", "Ghost the selection", Actions.GhostSelection),
        action("Isolate", "Hide everything else", Actions.Isolate),
      ]),
      action("Fit to selection", "Frame the selection", Actions.FitSelection),
    ]);
  }

  show(summary, tree) {
    if (summary.occurrences === 0) {
      this.element.hidden = true;
      return;
    }
    const mode = ["FACE", "BODY", "PART"][Math.min(summary.mode, 2)];
    this.#kind.textContent = summary.occurrences > 1 ? `${mode} · ${formatNumber(summary.occurrences)} BODIES` : mode;
    if (summary.node >= 0 && tree.documents[summary.document]?.model) {
      this.#name.textContent = tree.name(summary.document, summary.node);
      this.#path.textContent = tree.path(summary.document, summary.node);
    } else {
      this.#name.textContent = "Selection";
      this.#path.textContent = "";
    }
    this.#faces.textContent = formatNumber(summary.faces);
    this.#bodies.textContent = formatNumber(summary.occurrences);
    this.element.hidden = false;
  }
}

// ---- The toolbar ------------------------------------------------------------------------------

export class Toolbar {
  #needSelection = [];
  #needModel = [];

  constructor({ onAction, onMode, onProjection, onShortcuts, onLook, onSection, onOpen }) {
    this.pick = segmented(["Face", "Body", "Part"], {
      help: "What a click selects (1, 2, 3)",
      tips: ["Clicks select single faces (1)", "Clicks select whole bodies (2)", "Clicks select whole parts (3)"],
      onChange: onMode,
    });
    this.projection = segmented(["Ortho", "Persp"], { help: "Projection (P)", onChange: (i) => onProjection(i === 1) });
    const act = (a) => () => onAction(a);
    const hide = pill({ icon: icons.eyeSlash, help: "Hide the selection (hold H and click)", onPress: act(Actions.HideSelection) });
    const ghost = pill({ icon: icons.ghost, help: "Ghost the selection (hold G and click)", onPress: act(Actions.GhostSelection) });
    const isolate = pill({ title: "Isolate", help: "Hide everything else (I)", onPress: act(Actions.Isolate) });
    const showAll = pill({ icon: icons.eye, help: "Show everything (⇧H)", onPress: act(Actions.ShowAll) });
    const undo = pill({ icon: icons.undo, help: "Undo (⌘Z)", onPress: act(Actions.Undo) });
    const redo = pill({ icon: icons.redo, help: "Redo (⇧⌘Z)", onPress: act(Actions.Redo) });
    const seeThrough = pill({ icon: icons.seeThrough, help: "See through the housing (T)", onPress: act(Actions.ToggleSeeThrough) });
    const cut = pill({ icon: icons.scissors, help: "Section (S)", onPress: onSection });
    const look = pill({ icon: icons.palette, help: "Style and colour", onPress: onLook });
    const views = pill({ icon: icons.cube, help: "Standard views", onPress: () => this.#toggleViews(views) });
    const fit = pill({ icon: icons.fit, help: "Fit to view (F)", onPress: act(Actions.Fit) });
    const open = pill({ icon: icons.open, help: "Open a file (⌘O)", onPress: onOpen });
    const shortcuts = pill({ title: "?", style: "ghostly", help: "Shortcuts (?)", onPress: onShortcuts });
    this.#needSelection = [hide, ghost, isolate];
    this.#needModel = [showAll, fit, views, undo, redo, seeThrough];
    this.viewsMenu = el("div.menu.views", { hidden: true, role: "menu" }, [
      ["Front", Actions.ViewFront], ["Back", Actions.ViewBack], ["Left", Actions.ViewLeft],
      ["Right", Actions.ViewRight], ["Top", Actions.ViewTop], ["Bottom", Actions.ViewBottom],
      ["Isometric", Actions.ViewIso],
    ].map(([title, a]) => el("button", { type: "button", role: "menuitem", text: title,
                                          onclick: () => { this.viewsMenu.hidden = true; onAction(a); } })));
    const rule = () => el("span.rule");
    this.element = el("nav.card.toolbar", { "aria-label": "Tools" }, [
      open, rule(), this.pick.element, rule(), hide, ghost, isolate, showAll, rule(), undo, redo, rule(),
      seeThrough, cut, look, this.projection.element, views, fit, shortcuts,
    ]);
    this.update({ hasSelection: false, hasModel: false });
  }

  // The standard views, in a menu above their button, outside the toolbar so nothing clips it.
  #toggleViews(button) {
    const menu = this.viewsMenu;
    menu.hidden = !menu.hidden;
    if (menu.hidden) return;
    const rect = button.getBoundingClientRect();
    menu.style.left = `${Math.min(rect.left, innerWidth - menu.offsetWidth - 8)}px`;
    menu.style.top = `${rect.top - menu.offsetHeight - 10}px`;
  }

  update({ hasSelection, hasModel }) {
    for (const b of this.#needSelection) b.disabled = !hasSelection;
    for (const b of this.#needModel) b.disabled = !hasModel;
  }
}

// ---- The masthead -----------------------------------------------------------------------------

export class Masthead {
  #title = el("div.title");
  #subtitle = el("div.subtitle");
  #bar = el("div.progress", { hidden: true }, [el("div.fill")]);
  element = el("header.masthead", { hidden: true }, [this.#title, this.#subtitle, this.#bar]);

  show(title, subtitle) {
    this.#title.textContent = title;
    this.#subtitle.textContent = subtitle;
    this.element.hidden = false;
  }

  hide() { this.element.hidden = true; }

  setProgress(fraction) {
    this.#bar.hidden = fraction === null;
    if (fraction !== null) this.#bar.firstChild.style.width = `${Math.round(fraction * 100)}%`;
  }

  setOnDark(dark) { this.element.classList.toggle("on-dark", dark); }
}

// ---- The status card --------------------------------------------------------------------------

export class StatusCard {
  #heading = el("h2");
  #body = el("p");
  #open;
  element;

  constructor(onOpen) {
    this.#open = pill({ title: "Choose a file", onPress: onOpen });
    this.element = el("section.card.status", { hidden: true, role: "status" }, [this.#heading, this.#body, this.#open]);
  }

  showInvitation() {
    this.#show("Nothing open yet", "Drop a STEP, STL or cache file here, or press ⌘O to choose one.", true);
  }
  showLoading(name) { this.#show(`Opening ${name}`, "This only takes a moment."); }
  showFailure(message) { this.#show("That did not open", message, true); }
  hide() { this.element.hidden = true; }

  #show(heading, body, offerOpen = false) {
    this.#heading.textContent = heading;
    this.#body.textContent = body;
    this.#open.hidden = !offerOpen;
    this.element.hidden = false;
  }
}

// ---- Small overlays ---------------------------------------------------------------------------

export class HoldHint {
  element = el("div.chip.hint", { hidden: true, "aria-live": "polite" });

  show(tool) {
    const hints = {
      [Tools.Hide]: ["H", "Click to hide. Click again to peel the next layer."],
      [Tools.Ghost]: ["G", "Click to ghost. Click a ghost to bring it back."],
    };
    const hint = hints[tool];
    this.element.hidden = !hint;
    if (hint) this.element.replaceChildren(keyCap(hint[0]), el("span", { text: hint[1] }));
  }
}

// A label naming what is under the pointer, beside it.
export class HoverChip {
  element = el("div.chip.hover", { hidden: true });

  show(name, face, x, y) {
    if (name === null || x === null) {
      this.element.hidden = true;
      return;
    }
    this.element.replaceChildren(el("span.name", { text: name }), el("span.face", { text: `face ${face + 1}` }));
    this.element.hidden = false;
    const width = this.element.offsetWidth;
    this.element.style.left = `${Math.min(x + 16, innerWidth - width - 8)}px`;
    this.element.style.top = `${Math.max(y + 14, 8)}px`;
  }
}

export class DropZone {
  element = el("div.dropzone", { hidden: true }, [
    el("div.chip", { text: "Drop to open. Hold ⌘ to add to what is shown." }),
  ]);
}

export class ShortcutsSheet {
  element;

  constructor() {
    const entries = [
      [["Click"], "Select. Shift or ⌘ adds"],
      [["1", "2", "3"], "Clicks select faces, bodies or parts"],
      [["H", "Click"], "Hide what you click"],
      [["G", "Click"], "Ghost, or bring back a ghost"],
      [["⇧H"], "Show everything"],
      [["⇧G"], "Unghost everything"],
      [["I"], "Isolate the selection"],
      [["⌘", "Z"], "Undo, with ⇧ to redo"],
      [["F"], "Fit to view"],
      [["P"], "Orthographic or perspective"],
      [["T"], "See through the housing"],
      [["S"], "Section across X"],
      [["Esc"], "Clear the selection"],
      [["⌘O"], "Open a file"],
      [["?"], "This sheet"],
    ];
    const grid = el("div.grid", {}, entries.flatMap(([keys, meaning]) => [
      el("div.keys", {}, keys.map(keyCap)), el("div", { text: meaning }),
    ]));
    this.element = el("div.sheet", { hidden: true, onclick: () => { this.element.hidden = true; } }, [
      el("section.card", { onclick: (e) => e.stopPropagation() }, [el("h2", { text: "Shortcuts" }), grid]),
    ]);
  }
}
