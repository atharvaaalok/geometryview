// The assembly tree of everything open, with search, after the Mac's StructureCard and TreeRows.
// Rows come from walking nodes depth first and jumping over each collapsed subtree, so the work
// grows with what is shown; and only the rows in view are made into elements, so a tree of a
// million nodes scrolls as one of ten.

import { Actions, NodeActions, NodeKinds } from "./engine.js";
import { iconNode, icons } from "./icons.js";
import { el, pill, segmented, VirtualList } from "./ui.js";

// A row's height, as the Mac's; a phone's are taller, for fingers.
const rowHeight = () => (document.body.classList.contains("phone") ? 30 : 28);
const kIndent = 14;
const kNoNode = 0xFFFFFFFF;
const kInk = [0x23, 0x1e, 0x16];
const kDocumentRow = -1;

// The visible rows across all open documents.
export class TreeRows {
  documents = [];
  rows = []; // { document, node, depth }, node kDocumentRow for a file's own row

  reset() {
    this.documents = [];
    this.rows = [];
  }

  addDocument(name) {
    this.documents.push({ name, model: null, depths: null, partCounts: null,
                          expanded: new Set(), documentExpanded: true });
    this.#rebuild();
  }

  setModel(document, model) {
    const d = this.documents[document];
    if (!d) return;
    // A parent always comes before its children, so one pass down gives every depth and one pass
    // back up every count of parts.
    const depths = new Uint16Array(model.length);
    const parts = new Uint32Array(model.length);
    for (let i = 0; i < model.length; ++i) {
      if (model[i].parent >= 0) depths[i] = depths[model[i].parent] + 1;
    }
    for (let i = model.length - 1; i >= 0; --i) {
      if (model[i].kind === NodeKinds.Part) parts[i] += 1;
      if (model[i].parent >= 0) parts[model[i].parent] += parts[i];
    }
    Object.assign(d, { model, depths, partCounts: parts });
    // Roots start open, and a root with few children opens its sub-assemblies too.
    for (let i = 0; i < model.length; ++i) {
      if (model[i].parent >= 0) continue;
      d.expanded.add(i);
      const children = [];
      for (let child = i + 1; child < model[i].end && children.length <= 12; child = model[child].end) {
        children.push(child);
      }
      if (children.length > 12) continue;
      for (const child of children) if (model[child].kind === NodeKinds.Assembly) d.expanded.add(child);
    }
    this.#rebuild();
  }

  // A file's own row heads its tree when several files are open, or while its tree is on its way.
  #headed(d) { return this.documents.length > 1 || !d.model; }

  hasChildren(row) {
    const d = this.documents[row.document];
    if (row.node === kDocumentRow) return (d.model?.length ?? 0) > 0;
    return d.model !== null && d.model[row.node].end > row.node + 1;
  }

  isExpanded(row) {
    const d = this.documents[row.document];
    return row.node === kDocumentRow ? d.documentExpanded : d.expanded.has(row.node);
  }

  toggle(row) {
    const d = this.documents[row.document];
    if (row.node === kDocumentRow) d.documentExpanded = !d.documentExpanded;
    else if (d.expanded.has(row.node)) d.expanded.delete(row.node);
    else d.expanded.add(row.node);
    this.#rebuild();
  }

  // Opens every ancestor of a node and returns its row's index.
  reveal(document, node) {
    const d = this.documents[document];
    if (!d?.model || node >= d.model.length) return -1;
    d.documentExpanded = true;
    for (let parent = d.model[node].parent; parent >= 0; parent = d.model[parent].parent) {
      d.expanded.add(parent);
    }
    this.#rebuild();
    return this.rows.findIndex((row) => row.document === document && row.node === node);
  }

  name(document, node) {
    const d = this.documents[document];
    if (node === kDocumentRow) return d.name;
    if (!d.model) return "";
    const n = d.model[node];
    if (n.name) return n.name;
    if (n.kind === NodeKinds.Assembly) return "Unnamed assembly";
    if (n.kind === NodeKinds.Body) {
      // Its place among its parent's bodies, from 1.
      let ordinal = 1;
      if (n.parent >= 0) {
        for (let sibling = n.parent + 1; sibling < node; sibling = d.model[sibling].end) {
          if (d.model[sibling].kind === NodeKinds.Body) ordinal += 1;
        }
      }
      return `Body ${ordinal}`;
    }
    return "Unnamed part";
  }

  // The names of a node's ancestors, from the file down.
  path(document, node) {
    const d = this.documents[document];
    if (node === kDocumentRow || !d.model) return "";
    const names = [];
    for (let parent = d.model[node].parent; parent >= 0; parent = d.model[parent].parent) {
      names.push(this.name(document, parent));
    }
    names.push(d.name);
    return names.reverse().join(" / ");
  }

  // The parts in an assembly or a file, or ×N on a part made of N bodies.
  count(row) {
    const d = this.documents[row.document];
    if (!d.model) return "";
    if (row.node === kDocumentRow) {
      let parts = 0;
      d.model.forEach((n, i) => { if (n.parent < 0) parts += d.partCounts[i]; });
      return parts > 0 ? String(parts) : "";
    }
    const n = d.model[row.node];
    if (n.kind === NodeKinds.Assembly) return d.partCounts[row.node] > 0 ? String(d.partCounts[row.node]) : "";
    if (n.kind === NodeKinds.Part) {
      const bodies = n.end - row.node - 1;
      return bodies > 1 ? `×${bodies}` : "";
    }
    return "";
  }

  #rebuild() {
    const rows = [];
    this.documents.forEach((d, document) => {
      const headed = this.#headed(d);
      if (headed) rows.push({ document, node: kDocumentRow, depth: 0 });
      if ((headed && !d.documentExpanded) || !d.model) return;
      const indent = headed ? 1 : 0;
      for (let node = 0; node < d.model.length;) {
        rows.push({ document, node, depth: d.depths[node] + indent });
        node = d.expanded.has(node) ? node + 1 : d.model[node].end;
      }
    });
    this.rows = rows;
  }
}

// The card: Library and Structure switch, search, the rows, and a footer for what is hidden.
export class StructureCard {
  tree = new TreeRows();
  #engine;
  #list;
  #nothing;
  #search;
  #matchLabel;
  #footer;
  #hiddenLabel;
  #ghostedLabel;
  #showAll;
  #unghostAll;
  #matches = [];
  #matchIndex = 0;
  #states = []; // per document, one NodeVisibility per node
  #swatches = { colors: new Uint32Array(0), bases: new Uint32Array(0) };
  #selected = null; // { document, node } of the selection's focus row
  #highlighted = null; // from the pointer in the view
  #pointerRow = -1;

  constructor(engine, { onSwitch, onHide }) {
    this.#engine = engine;
    this.switcher = segmented(["Library", "Structure"], { selected: 1, help: "Library or structure",
                                                          onChange: (i) => onSwitch?.(i === 0) });
    this.#search = el("input.search", { type: "search", placeholder: "Search parts",
                                         "aria-label": "Search parts", spellcheck: "false" });
    this.#search.addEventListener("input", () => this.#searchChanged());
    this.#search.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && this.#matches.length > 0) {
        this.#matchIndex = (this.#matchIndex + 1) % this.#matches.length;
        this.#showMatch();
      } else if (e.key === "Escape") {
        this.clearSearch();
        this.#search.blur();
      }
    });
    this.#matchLabel = el("span.match");
    this.#list = new VirtualList({ role: "tree", rowHeight, count: () => this.tree.rows.length,
                                   row: (index) => this.#row(index) });
    this.#list.element.addEventListener("pointerleave", () => this.#hoverRow(-1));
    this.#hiddenLabel = el("span");
    this.#ghostedLabel = el("span");
    this.#showAll = pill({ title: "Show all", help: "Show everything, ghosts included (⇧H)",
                          onPress: () => engine.perform(Actions.ShowAll) });
    this.#unghostAll = pill({ title: "Unghost all", onPress: () => engine.perform(Actions.UnghostAll) });
    this.#footer = el("div.footer", { hidden: true }, [
      el("div.counts", {}, [this.#hiddenLabel, this.#ghostedLabel]),
      el("div.buttons", {}, [this.#showAll, this.#unghostAll]),
    ]);
    this.#nothing = el("p.nothing", { hidden: true, text: "Nothing open yet." });
    // On a desktop the card can be put away, for the whole window.
    const hide = pill({ icon: icons.sidebar, help: "Hide the sidebar (⌥S)", onPress: () => onHide?.() });
    hide.classList.add("desktop-only");
    this.element = el("section.card.structure", { "aria-label": "Structure" }, [
      el("div.header", {}, [this.switcher.element, hide]),
      this.#nothing,
      el("label.searchbox", {}, [el("span.glyph", { html: icons.search(14) }), this.#search, this.#matchLabel]),
      this.#list.element,
      this.#footer,
    ]);
  }

  // With nothing open, the card says so in place of the search and the tree.
  showNothing(nothing) {
    this.#nothing.hidden = !nothing;
    for (const part of this.element.querySelectorAll(".searchbox, .tree")) part.hidden = nothing;
  }

  reset() {
    this.tree.reset();
    this.#states = [];
    this.#selected = null;
    this.clearSearch();
    this.#render();
  }

  addDocument(name) {
    this.tree.addDocument(name);
    this.#render();
  }

  modelReady(document) {
    const model = this.#engine.model(document);
    if (model) this.tree.setModel(document, model);
    this.#states[document] = this.#engine.nodeStates(document);
    this.#render();
  }

  // The selection's focus, from the engine: revealed and marked, or nothing.
  reveal(document, node) {
    if (node === kNoNode) {
      this.#selected = null;
      this.#render();
      return;
    }
    this.#selected = { document, node };
    const index = this.tree.reveal(document, node);
    if (index >= 0) this.#scrollToRow(index);
    this.#render();
  }

  // The part under the pointer in the view.
  highlight(document, node) {
    this.#highlighted = node === kNoNode ? null : { document, node };
    this.#schedule();
  }

  visibilityChanged(hidden, ghosted) {
    this.tree.documents.forEach((_, document) => {
      this.#states[document] = this.#engine.nodeStates(document);
    });
    this.#hiddenLabel.textContent = hidden > 0 ? `${hidden.toLocaleString("en-US")} ${hidden === 1 ? "body" : "bodies"} hidden` : "";
    this.#ghostedLabel.textContent = ghosted > 0 ? `${ghosted.toLocaleString("en-US")} ${ghosted === 1 ? "body" : "bodies"} ghosted` : "";
    this.#showAll.hidden = hidden === 0;
    this.#unghostAll.hidden = ghosted === 0;
    this.#footer.hidden = hidden === 0 && ghosted === 0;
    this.#schedule();
  }

  looksChanged() {
    this.#swatches = this.#engine.swatches();
    this.#schedule();
  }

  clearSearch() {
    this.#search.value = "";
    this.#matches = [];
    this.#matchLabel.textContent = "";
  }

  focusSearch() { this.#search.focus(); }

  // ---- Search ------------------------------------------------------------------------------

  #searchChanged() {
    const query = this.#search.value.trim().toLowerCase();
    this.#matches = [];
    this.#matchIndex = 0;
    if (!query) {
      this.#matchLabel.textContent = "";
      return;
    }
    this.tree.documents.forEach((d, document) => {
      d.model?.forEach((n, node) => {
        if (n.name.toLowerCase().includes(query)) this.#matches.push({ document, node });
      });
    });
    this.#showMatch();
  }

  #showMatch() {
    const total = this.#matches.length;
    this.#matchLabel.textContent = total === 0 ? "no match" : `${this.#matchIndex + 1} of ${total}`;
    const match = this.#matches[this.#matchIndex];
    if (!match) return;
    this.#selected = match;
    const index = this.tree.reveal(match.document, match.node);
    if (index >= 0) this.#scrollToRow(index);
    this.#render();
  }

  // ---- Rows --------------------------------------------------------------------------------

  #scrollToRow(index) { this.#list.scrollToRow(index); }
  #schedule() { this.#list.schedule(); }
  #render() { this.#list.render(); }

  #state(row) {
    if (row.node === kDocumentRow) return null;
    return this.#states[row.document]?.[row.node] ?? 0;
  }

  #row(index) {
    const row = this.tree.rows[index];
    const isDocument = row.node === kDocumentRow;
    const state = this.#state(row);
    const d = this.tree.documents[row.document];
    const kind = isDocument ? null : d.model?.[row.node]?.kind;
    const element = el("div.row", { role: "treeitem", "data-index": index });
    element.style.paddingLeft = `${8 + row.depth * kIndent}px`;
    const selected = this.#selected && this.#selected.document === row.document &&
                     this.#selected.node === row.node;
    const highlighted = this.#highlighted && !isDocument &&
                        this.#highlighted.document === row.document && this.#highlighted.node === row.node;
    element.classList.toggle("selected", Boolean(selected));
    element.classList.toggle("highlighted", Boolean(highlighted) || index === this.#pointerRow);
    element.classList.toggle("hidden-node", state === 1);
    element.classList.toggle("ghosted-node", state === 2);

    if (this.tree.hasChildren(row)) {
      const chevron = el("button.chevron", { type: "button", "aria-label": "Expand" }, [iconNode("chevron", 12)]);
      chevron.classList.toggle("open", this.tree.isExpanded(row));
      chevron.addEventListener("click", (e) => {
        e.stopPropagation();
        this.tree.toggle(row);
        this.#render();
      });
      element.append(chevron);
    } else {
      element.append(el("span.chevron"));
    }
    if (isDocument) {
      element.append(el("span.glyph", {}, [iconNode("file", 14)]));
    } else if (kind === NodeKinds.Assembly) {
      element.append(el("span.glyph", {}, [iconNode("assembly", 16)]));
    } else {
      const swatch = el(`span.swatch${kind === NodeKinds.Body ? ".small" : ""}`);
      const chip = this.#chipOf(row);
      if (chip !== null) swatch.style.backgroundImage = chip;
      element.append(swatch);
    }
    element.append(el("span.name", { text: this.tree.name(row.document, row.node) }));
    if (!isDocument) {
      const ghosted = state === 2, hidden = state === 1;
      const actions = el("span.actions", {}, [
        this.#rowButton(ghosted ? "Unghost" : "Ghost", "ghost", ghosted, () =>
          this.#engine.nodeAction(row.document, row.node, ghosted ? NodeActions.Unghost : NodeActions.Ghost)),
        this.#rowButton(hidden ? "Show" : "Hide", hidden ? "eyeSlash" : "eye", hidden, () =>
          this.#engine.nodeAction(row.document, row.node, hidden ? NodeActions.Show : NodeActions.Hide)),
        this.#rowButton("Show only this, framed", "scope", false, () =>
          this.#engine.nodeAction(row.document, row.node, NodeActions.Isolate)),
      ]);
      element.append(actions);
    }
    const mark = state === 1 ? "eyeSlash" : state === 2 ? "ghost" : state === 3 ? "seeThrough" : null;
    if (mark) element.append(el("span.mark", {}, [iconNode(mark, 12)]));
    else {
      const count = this.tree.count(row);
      if (count) element.append(el("span.count", { text: count }));
    }

    element.addEventListener("click", (e) => {
      if (isDocument) {
        this.tree.toggle(row);
        this.#render();
        return;
      }
      this.#selected = { document: row.document, node: row.node };
      this.#engine.selectNode(row.document, row.node, e.shiftKey || e.metaKey || e.ctrlKey);
      this.#render();
    });
    element.addEventListener("pointerenter", () => this.#hoverRow(index));
    return element;
  }

  #rowButton(help, icon, on, press) {
    const button = el(`button.rowbutton${on ? ".on" : ""}`, { type: "button", title: help,
                                                            "aria-label": help }, [iconNode(icon, 13)]);
    button.addEventListener("click", (e) => {
      e.stopPropagation();
      press();
    });
    return button;
  }

  #hoverRow(index) {
    if (index === this.#pointerRow) return;
    this.#pointerRow = index;
    const row = this.tree.rows[index];
    if (!row || row.node === kDocumentRow) this.#engine.hoverNode(0, kNoNode);
    else this.#engine.hoverNode(row.document, row.node);
  }

  // The colour the part is drawn in, from its first occurrence's swatch, as its chip: the colour,
  // and past a diagonal 55% of the way across, the colour 16% toward ink.
  #chipOf(row) {
    const d = this.tree.documents[row.document];
    const n = d.model?.[row.node];
    if (!n || n.occurrenceEnd <= n.firstOccurrence) return null;
    const base = this.#swatches.bases[row.document];
    const color = base === undefined ? 0 : this.#swatches.colors[base + n.firstOccurrence] ?? 0;
    if (color === 0) return null;
    const rgb = [color & 255, (color >>> 8) & 255, (color >>> 16) & 255];
    const shade = rgb.map((c, i) => Math.round(c * 0.84 + kInk[i] * 0.16));
    return `linear-gradient(to bottom right, rgb(${rgb}) 55%, rgb(${shade}) 55%)`;
  }
}
