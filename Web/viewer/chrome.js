// The cards and overlays around the view, after the Mac app's Chrome folder: the masthead, the
// toolbar, the look, section and selection cards, the status card, the hold hint, the hover chip,
// the drop zone and the shortcuts sheet.

import { Actions, Tools } from "./engine.js";
import { iconNode, icons } from "./icons.js";
import { foldPlan, ruleShows } from "./toolbar-fold.js";
import { el, formatNumber, keyCap, pill, segmented, slider } from "./ui.js";

// ---- The look ---------------------------------------------------------------------------------

// As engine.h numbers them. onDark: the sheet is dark, so the chrome drawn on it turns light.
const row = (title, usesColour, castsShadow, grain, wobble, onDark) =>
  ({ title, usesColour, castsShadow, grain, wobble, onDark });
export const Styles = [
  row("Illustration", true, true, true, true, false),
  row("Metal", true, true, false, false, false),
  row("Clay", true, true, false, false, false),
  row("Blueprint", false, false, true, false, true),
  row("Technical", false, false, false, false, false),
  row("Studio", true, true, false, false, true),
];
const Colours = [
  { title: "Material", summary: "Colours follow the material each name suggests." },
  { title: "Parts", summary: "Each part gets a colour of its own." },
  { title: "Mono", summary: "Every part in one neutral colour." },
  { title: "File", summary: "The colours stored in the file." },
];
const Ghosts = [{ title: "Shell", value: 1 }, { title: "Lines", value: 2 }, { title: "Both", value: 3 }];
const kWobbleSteps = [0, 1, 3, 5];

// How the model is drawn, as chosen; remembered in this browser between visits. Until the person
// chooses, it is the engine's default, which every app shares.
export class Look {
  style = 0; colour = 1; ghosts = 1; shadow = true; grain = true; wobble = 0;

  // `defaults` is the engine's default look, as Engine.defaultLook gives it.
  static saved(defaults) {
    const look = new Look();
    if (defaults) {
      Object.assign(look, { style: defaults.style, colour: defaults.colorMode, ghosts: defaults.ghostStyle,
                            shadow: defaults.castShadow, grain: defaults.grain > 0, wobble: defaults.wobble });
    }
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
  #tools = [];   // { key, element, group }, in order
  #rules = [];   // the rule after each group but the last
  #folded = new Set();
  #handlers;
  #more;

  constructor({ onAction, onMode, onProjection, onShortcuts, onLook, onSection, onOpen, onConnect, canConnect,
                onParts }) {
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
    // The two ways in, as on the first page and in the Library.
    const open = pill({ icon: icons.plus, help: "Open files (⌘O)", onPress: onOpen });
    const connect = pill({ icon: icons.folderPlus, help: "Connect a folder (⌥⌘O)", onPress: onConnect });
    connect.hidden = !canConnect;
    // On a phone the tree is a sheet over the view, opened from here; on a desktop, the sidebar
    // brought back once it is put away.
    const onPhone = document.body.classList.contains("phone");
    const parts = pill({ icon: onPhone ? icons.parts : icons.sidebar,
                         help: onPhone ? "Parts and library" : "Show the sidebar (⌥S)", onPress: onParts });
    parts.classList.add("pane-pill");
    const shortcuts = pill({ title: "?", style: "ghostly", help: "Shortcuts (?)", onPress: onShortcuts });
    // A phone has the selection card's own buttons for these, and no keyboard.
    for (const button of [hide, ghost, isolate, shortcuts]) button.classList.add("desktop-only");
    this.#needSelection = [hide, ghost, isolate];
    this.#needModel = [showAll, fit, views, undo, redo, seeThrough];
    this.viewsMenu = el("div.menu.views", { hidden: true, role: "menu" }, [
      ["Front", Actions.ViewFront], ["Back", Actions.ViewBack], ["Left", Actions.ViewLeft],
      ["Right", Actions.ViewRight], ["Top", Actions.ViewTop], ["Bottom", Actions.ViewBottom],
      ["Isometric", Actions.ViewIso],
    ].map(([title, a]) => el("button", { type: "button", role: "menuitem", text: title,
                                          onclick: () => { this.viewsMenu.hidden = true; onAction(a); } })));
    this.#handlers = { onAction, onProjection, onShortcuts, onLook, onSection, onOpen, onConnect };
    this.#more = pill({ icon: icons.more, help: "More tools", onPress: () => this.#toggleMore() });
    this.#more.classList.add("folded");
    this.#more.dataset.tool = "more";
    this.moreMenu = el("div.menu.more", { hidden: true, role: "menu" });
    const groups = [
      [["parts", parts], ["open", open], ["connect", connect]],
      [["pick", this.pick.element]],
      [["hide", hide], ["ghost", ghost], ["isolate", isolate], ["showAll", showAll]],
      [["undo", undo], ["redo", redo]],
      [["seeThrough", seeThrough], ["section", cut], ["look", look], ["projection", this.projection.element],
       ["views", views], ["fit", fit], ["shortcuts", shortcuts]],
    ];
    const children = [];
    groups.forEach((tools, group) => {
      if (group > 0) {
        const rule = el("span.rule");
        this.#rules.push(rule);
        children.push(rule);
      }
      for (const [key, element] of tools) {
        element.dataset.tool = key;
        this.#tools.push({ key, element, group });
        children.push(element);
      }
    });
    children.push(this.#more);
    this.element = el("nav.card.toolbar", { "aria-label": "Tools" }, children);
    this.update({ hasSelection: false, hasModel: false });
  }

  // Folds the least used tools into More until the toolbar fits `available` pixels, by the rule in
  // toolbar-fold.js. Measures each tool as shown, so it runs only when the space changes.
  fit(available) {
    for (const tool of this.#tools) tool.element.classList.remove("folded");
    const items = this.#tools.map(({ key, element, group }) => ({ key, group, width: element.offsetWidth }));
    this.#folded = foldPlan(items, available);
    for (const tool of this.#tools) tool.element.classList.toggle("folded", this.#folded.has(tool.key));
    this.#rules.forEach((rule, group) => rule.classList.toggle("folded", !ruleShows(items, this.#folded, group)));
    this.#more.classList.toggle("folded", this.#folded.size === 0);
    if (this.#folded.size === 0) this.moreMenu.hidden = true;
  }

  // The folded tools, as rows in toolbar order: the views under a heading of their own.
  #toggleMore() {
    const menu = this.moreMenu;
    if (!menu.hidden) {
      menu.hidden = true;
      return;
    }
    const h = this.#handlers;
    const tool = (key) => this.#tools.find((t) => t.key === key).element;
    const row = (icon, title, press, { disabled = false, checked = false } = {}) => {
      const button = el("button", { type: "button", role: "menuitem", disabled },
                        [el("span.icon", {}, icon ? [iconNode(icon, 15)] : []), el("span", { text: title }),
                         el("span.check", {}, checked ? [iconNode("check", 13)] : [])]);
      button.addEventListener("click", () => { menu.hidden = true; press(); });
      return button;
    };
    const action = (key, icon, title, a) => row(icon, title, () => h.onAction(a), { disabled: tool(key).disabled });
    const rows = {
      open: () => [row("plus", "Open files", h.onOpen)],
      connect: () => (tool("connect").hidden ? [] : [row("folderPlus", "Connect a folder", h.onConnect)]),
      hide: () => [action("hide", "eyeSlash", "Hide the selection", Actions.HideSelection)],
      ghost: () => [action("ghost", "ghost", "Ghost the selection", Actions.GhostSelection)],
      isolate: () => [action("isolate", "scope", "Isolate", Actions.Isolate)],
      showAll: () => [action("showAll", "eye", "Show everything", Actions.ShowAll)],
      undo: () => [action("undo", "undo", "Undo", Actions.Undo)],
      redo: () => [action("redo", "redo", "Redo", Actions.Redo)],
      seeThrough: () => [action("seeThrough", "seeThrough", "See through", Actions.ToggleSeeThrough)],
      section: () => [row("scissors", "Section", h.onSection)],
      look: () => [row("palette", "Look", h.onLook)],
      projection: () => ["Orthographic", "Perspective"].map((title, i) => row(null, title, () => {
        this.projection.selected = i;
        h.onProjection(i === 1);
      }, { checked: this.projection.selected === i })),
      views: () => [el("div.heading", { text: "Views" }),
                    ...[["Front", Actions.ViewFront], ["Back", Actions.ViewBack], ["Left", Actions.ViewLeft],
                        ["Right", Actions.ViewRight], ["Top", Actions.ViewTop], ["Bottom", Actions.ViewBottom],
                        ["Isometric", Actions.ViewIso]]
                      .map(([title, a]) => row(null, title, () => h.onAction(a), { disabled: tool("views").disabled }))],
      fit: () => [action("fit", "fit", "Fit to view", Actions.Fit)],
      shortcuts: () => [row(null, "Shortcuts", h.onShortcuts)],
    };
    const items = this.#tools.filter((t) => this.#folded.has(t.key)).flatMap((t) => rows[t.key]?.() ?? []);
    menu.replaceChildren(...items);
    menu.hidden = false;
    this.#place(menu, this.#more);
  }

  // A menu above its button, kept 8 pixels inside the window.
  #place(menu, button) {
    const rect = button.getBoundingClientRect();
    menu.style.left = `${Math.max(8, Math.min(rect.left, innerWidth - menu.offsetWidth - 8))}px`;
    menu.style.top = `${rect.top - menu.offsetHeight - 10}px`;
  }

  // The standard views, in a menu above their button, outside the toolbar so nothing clips it.
  #toggleViews(button) {
    const menu = this.viewsMenu;
    menu.hidden = !menu.hidden;
    if (menu.hidden) return;
    this.#place(menu, button);
  }

  update({ hasSelection, hasModel }) {
    for (const b of this.#needSelection) b.disabled = !hasSelection;
    for (const b of this.#needModel) b.disabled = !hasModel;
  }
}

// ---- The masthead -----------------------------------------------------------------------------

// What is open, with a button that closes it, a line about it, and import progress.
export class Masthead {
  #title = el("div.title");
  #close = pill({ icon: icons.close, help: "Close", style: "ghostly", onPress: () => this.onClose?.() });
  #subtitle = el("div.subtitle");
  #bar = el("div.progress", { hidden: true }, [el("div.fill")]);
  element = el("header.masthead", { hidden: true },
               [el("div.title-row", {}, [this.#title, this.#close]), this.#subtitle, this.#bar]);
  onClose = null;

  show(title, subtitle) {
    this.#title.textContent = title;
    this.#subtitle.textContent = subtitle;
    this.element.hidden = false;
  }

  hide() { this.element.hidden = true; }

  setProgress(fraction) {
    this.#bar.hidden = fraction === null;
    if (fraction !== null) this.#bar.firstChild.style.transform = `translateX(${(fraction - 1) * 100}%)`;
  }

  setOnDark(dark) { this.element.classList.toggle("on-dark", dark); }
}

// ---- The status card --------------------------------------------------------------------------

// A size as people read it: 57 KB, 8.1 MB, 25 MB.
function formatBytes(bytes) {
  const [value, unit] = bytes >= 1e6 ? [bytes / 1e6, "MB"] : [Math.max(1, bytes / 1e3), "KB"];
  return `${value < 10 ? value.toFixed(1).replace(/\.0$/, "") : Math.round(value)} ${unit}`;
}

export class StatusCard {
  #heading = el("h2");
  #body = el("p");
  #buttons;
  #samples = el("div.samples", { hidden: true });
  #offering = false;
  #invitation;
  element;

  // The two ways in: opening files and, where this browser can reach folders, connecting one.
  constructor({ onOpen, onConnect, canConnect, phone }) {
    const open = pill({ icon: icons.plus, title: "Open files", help: "Open STEP, STL or cache files (⌘O)", onPress: onOpen });
    const connect = pill({ icon: icons.folderPlus, title: "Connect a folder", help: "Connect a folder (⌥⌘O)",
                           onPress: onConnect });
    connect.hidden = !canConnect;
    this.#buttons = el("div.buttons", {}, [open, connect]);
    this.#invitation = phone ? (canConnect ? "Open STEP or STL files, or connect a folder." : "Open STEP or STL files.")
                             : (canConnect ? "Drop STEP or STL files to open them, or a folder to connect it."
                                           : "Drop STEP or STL files here to open them.");
    this.element = el("section.card.status", { hidden: true, role: "status" },
                      [this.#heading, this.#body, this.#buttons, this.#samples]);
  }

  // Models to try, offered beside the two ways in: [{ name, size, about }], and what picking one does.
  setSamples(samples, onPick) {
    this.#samples.replaceChildren(el("p.heading", { text: "Or try a sample" }), ...samples.map((sample) =>
      el("button.sample", { type: "button", title: sample.about, onclick: () => onPick(sample) },
         [el("span.name", { text: sample.name }), el("span.size", { text: formatBytes(sample.size) })])));
    this.#samples.dataset.count = samples.length;
    this.#samples.hidden = !this.#offering || samples.length === 0;
  }

  showInvitation() {
    this.#show("Nothing open yet", this.#invitation, true);
  }
  showLoading(name) { this.#show(`Opening ${name}`, "This only takes a moment."); }
  showDownloading(name, done, total) {
    this.#show(`Downloading ${name}`, `${formatBytes(done)} of ${formatBytes(total)}.`);
  }
  showImporting(name, done, total) {
    this.#show(`Importing ${name}`, total > 0 ? `${done} of ${total} pieces meshed.`
                                              : "Reading its structure…");
  }
  showFailure(message) { this.#show("That did not open", message, true); }
  hide() { this.element.hidden = true; }

  #show(heading, body, offerOpen = false) {
    this.#heading.textContent = heading;
    this.#body.textContent = body;
    this.#buttons.hidden = !offerOpen;
    this.#offering = offerOpen;
    this.#samples.hidden = !offerOpen || !Number(this.#samples.dataset.count);
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
    this.element.classList.toggle("hiding", tool === Tools.Hide);
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
    el("div.words", { text: "Drop files to open them, or a folder to connect it. Hold ⌘ to add to what is shown." }),
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
      [["⇧H"], "Show everything, ghosts included"],
      [["⇧G"], "Unghost everything"],
      [["I"], "Isolate the selection"],
      [["⌘", "Z"], "Undo, with ⇧ to redo"],
      [["F"], "Fit to view"],
      [["P"], "Orthographic or perspective"],
      [["T"], "See through the housing"],
      [["S"], "Section across X"],
      [["Esc"], "Close a card, or clear the selection"],
      [["⌘O"], "Open a file"],
      [["⌥S"], "Hide or show the sidebar"],
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
