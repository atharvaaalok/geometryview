// The universal viewer's page: the engine on a full-window canvas, and the chrome around it, laid
// out as the Mac app lays out its window.
//
// viewer/?model=<fixture>        opens <fixtures>/<fixture>.geoblob (fixtures= moves the folder)
// viewer/?sample=<id>            opens a sample from samples/index.json, where the copy has them
// viewer/?test=idle&label=...    runs a scripted check and posts its report (see runTest)

import { Actions, Engine, Events, NodeKinds, Tools } from "./engine.js";
import { ViewportInput } from "./input.js";
import { StructureCard } from "./structure.js";
import { LibraryCard, cachedImport, forgetKept, keptFile, keptSource, libraryEntries,
         setCacheVersion } from "./library.js";
import { keyOf } from "./library-rows.js";
import { LibraryStore } from "./library-store.js";
import { Importers } from "./importers.js";
import { Gallery } from "./gallery.js";
import { GalleryToolbar } from "./gallery-toolbar.js";
import { kContentInsets as kGalleryInsets } from "./gallery-view.js";
import { evict as evictImports, forget as forgetKeptKey, forgetNamed, keptImport, keptSource as keptSourceOf,
         keyOf as keptKeyOf, persistOnce } from "./kept.js";
import { DropZone, HoldHint, HoverChip, Look, LookCard, Masthead, SectionCard, SelectionCard,
         ShortcutsSheet, StatusCard, Styles, Toolbar } from "./chrome.js";
import { el } from "./ui.js";

const params = new URLSearchParams(location.search);
// Errors, kept for the scripted checks' reports.
const errors = [];
addEventListener("error", (e) => errors.push(`${e.message} at ${e.filename}:${e.lineno}`));
addEventListener("unhandledrejection", (e) => errors.push(String(e.reason?.stack ?? e.reason)));
// Served without the headers threads need, as GitHub Pages serves the page, a service worker of
// the page's own adds them. The page reloads once, to be served through it, before starting.
async function isolate() {
  if (self.crossOriginIsolated || !self.isSecureContext || !("serviceWorker" in navigator)) return;
  if (params.has("token") || window.webkit?.messageHandlers?.shell) return;
  try {
    await navigator.serviceWorker.register("isolation-worker.js");
    // Served through it already, but not isolated: this browser can't be, so go on without threads.
    if (navigator.serviceWorker.controller || sessionStorage.getItem("isolation-reload")) return;
    sessionStorage.setItem("isolation-reload", "1");
    await navigator.serviceWorker.ready;
  } catch {
    return;
  }
  location.reload();
  await new Promise(() => {});
}
await isolate();
const canvas = document.getElementById("viewport");
// A phone, or a check asking to be one (?phone=1).
const phone = params.get("phone") === "1" || /iPhone|iPad|Android/.test(navigator.userAgent) ||
              (navigator.maxTouchPoints > 1 && /Macintosh/.test(navigator.userAgent));
document.body.classList.toggle("phone", phone);

// The Mac window's measurements, in CSS pixels.
const kEdge = 18, kCardWidth = 300, kCardTop = 72, kCardBottom = 96;

const listeners = new Set();
const waitFor = (kind) => new Promise((resolve) => {
  const listener = (event) => {
    if (event.kind === kind || event.kind === Events.Failed) {
      listeners.delete(listener);
      resolve(event);
    }
  };
  listeners.add(listener);
});

// ---- The engine and the chrome ----------------------------------------------------------------

// Where the Library's folders and recent files come from.
const libraryStore = new LibraryStore({ backend: params.get("backend") ?? undefined });
const status = new StatusCard({ onOpen: () => chooseFile(false), onConnect: () => connectFolder(),
                                canConnect: libraryStore.canConnect, phone });
document.body.append(status.element);
// The engine may speak while it starts, before the chrome below exists to listen.
let chromeReady = false;

let engine;
try {
  engine = await Engine.start(canvas, {
    event: handle,
    refine: (token, name, level, definitions) => refineDocument(token, name, level, definitions),
    cancelRefinement: () => cancelRefinement(),
  }, { phone });
} catch (error) {
  status.showFailure(`This browser cannot run the viewer: ${error.message}${webgpuAdvice()}`);
  throw error;
}

// What to do where a browser offers no WebGPU and the way to it is known.
function webgpuAdvice() {
  const agent = navigator.userAgent;
  // Chrome on Linux has no adapter without Vulkan, which is off by default on NVIDIA GPUs at least.
  if (/Linux/.test(agent) && !/Android/.test(agent) && /Chrome\//.test(agent)) {
    return " Chrome on Linux needs Vulkan for WebGPU: turn on chrome://flags/#enable-vulkan, then " +
           "restart Chrome.";
  }
  return navigator.gpu ? "" : " Open it in a current Chrome, Edge or Safari.";
}
window.geometryView = engine; // for the console and the shell's checks
// ?plain leaves out the view cube and scale bar, for pictures.
if (params.has("plain")) engine.setOverlayShown(false);
setCacheVersion(engine.cacheVersion);
// From navigation to an engine ready to draw: the page, the module, the GPU and the overlay's text.
const startupMs = Math.round(performance.now());

let look = Look.saved(engine.defaultLook());
let perspective = false;
let names = [];
const progress = new Map();
let hovered = null; // { document, node, face }
// The tree and library card: a sheet opened when wanted on a phone; on a desktop open unless put
// away, which the page remembers.
const kSidebarHidden = "sidebar.hidden";
let panelOpen = !phone && !(() => {
  try { return localStorage.getItem(kSidebarHidden) === "1"; } catch { return false; }
})();
document.body.classList.toggle("sidebar-hidden", !phone && !panelOpen);
let showingLibrary = true; // which card the sidebar shows when open
// The gallery, made when a folder's is first shown (gallery.js).
let gallery = null;
let galleryShown = false;
let openedFromGallery = false;
// What the chrome covers of the view, in CSS pixels, as framing keeps clear of it.
let insets = { left: 0, top: 0, right: 0, bottom: 0 };
let pointer = null; // { x, y } in CSS pixels, while over the view

const masthead = new Masthead();
// In the gallery, × leaves it, as Esc does at the top; in the viewer it closes the files.
masthead.onClose = () => (galleryShown ? leaveGallery() : closeFiles());
const structure = new StructureCard(engine, { onSwitch: (show) => showLibrary(show),
                                              onHide: () => setPanelOpen(false) });
const library = new LibraryCard(libraryStore, {
  open: (row, add) => openFromFolder(row, add),
  reopen: (entry, add) => reopenRecent(entry, add),
  choose: () => chooseFile(false),
  connect: () => connectFolder(),
  switch: (show) => showLibrary(show),
  forget: (key) => forgetRecent(key),
  showFolder: (root, path) => showGallery(root, path),
  search: (query) => gallery?.setSearch(query),
  hide: () => setPanelOpen(false),
});
const selection = new SelectionCard((action) => engine.perform(action));
const lookCard = new LookCard(look, (next) => applyLook(next));
const sectionCard = new SectionCard((cuts) => engine.setSection(cuts));
const toolbar = new Toolbar({
  onAction: (action) => engine.perform(action),
  onMode: (mode) => { engine.setSelectionMode(mode); refreshSelection(); },
  onProjection: (persp) => {
    if (persp !== perspective) {
      perspective = persp;
      engine.perform(Actions.ToggleProjection);
    }
  },
  onShortcuts: () => { shortcuts.element.hidden = false; },
  onLook: () => toggleCard(lookCard),
  onSection: () => toggleCard(sectionCard),
  onOpen: () => chooseFile(false),
  onConnect: () => connectFolder(),
  canConnect: libraryStore.canConnect,
  // A phone's sheet shows the Library until something is open; a desktop's sidebar comes back as
  // it was.
  onParts: () => setPanelOpen(!panelOpen, phone ? names.length === 0 : showingLibrary),
});
const galleryToolbar = new GalleryToolbar({
  view: (view) => gallery?.setView(view),
  sheet: (change) => gallery?.setSheet(change),
  ladders: (change) => gallery?.setLadders(change),
  atlas: (change) => gallery?.atlas(change),
  catalogue: (change) => gallery?.catalogue(change),
  look: () => toggleCard(lookCard),
  poster: () => setGalleryPoster(!gallery?.poster),
  save: () => saveGalleryPoster(),
  savePDF: () => saveGalleryPDF(),
  // On a phone the Library is a sheet raised from the toolbar; on a desktop the sidebar comes back.
  library: () => setPanelOpen(!panelOpen, true),
});
const holdHint = new HoldHint();
const hoverChip = new HoverChip();
const dropZone = new DropZone();
const shortcuts = new ShortcutsSheet();
const picker = el("input", { type: "file", accept: ".step,.stp,.stl,.geoblob", multiple: true, hidden: true });
picker.addEventListener("change", () => {
  openFiles([...picker.files], picker.dataset.adding === "1");
  picker.value = "";
});
document.body.append(masthead.element, structure.element, library.element, selection.element,
                     lookCard.element, sectionCard.element, toolbar.element, toolbar.viewsMenu, toolbar.moreMenu,
                     galleryToolbar.element, galleryToolbar.posterBar,
                     holdHint.element,
                     hoverChip.element, dropZone.element, shortcuts.element, picker);

new ResizeObserver(() => engine.resize()).observe(canvas);
new ViewportInput(engine, canvas, {
  onCommand: run,
  onTool: (tool) => {
    holdHint.show(tool);
    canvas.style.cursor = tool === Tools.Select ? "" : "crosshair";
  },
  onPointer: (x, y) => {
    pointer = x === null ? null : { x, y };
    showHover();
  },
});
// While the gallery shows, its keys are its own; in the viewer, Esc goes back to the gallery the
// files came from once no card is open.
addEventListener("keydown", (e) => {
  // ⌥S puts the sidebar away or brings it back, in the viewer and the gallery alike; not while
  // typing, where it is a letter.
  if (!phone && e.altKey && e.code === "KeyS" && !e.metaKey && !e.ctrlKey &&
      !(e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement)) {
    e.preventDefault();
    e.stopImmediatePropagation();
    setPanelOpen(!panelOpen, galleryShown || showingLibrary);
    return;
  }
  if (galleryShown && e.key === "Escape" && dismissCard()) {
    // A card open over the gallery closes first.
    e.preventDefault();
    e.stopImmediatePropagation();
  } else if (galleryShown && gallery?.key(e)) {
    e.preventDefault();
    e.stopImmediatePropagation();
  } else if (!galleryShown && openedFromGallery && e.key === "Escape" && !dismissCard()) {
    e.preventDefault();
    e.stopImmediatePropagation();
    enterGallery();
  }
}, { capture: true });
// ⌘O opens files, ⇧⌘O adds them to what is shown, ⌥⌘O connects a folder: the Mac's keys.
addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.code === "KeyO") {
    e.preventDefault();
    if (e.altKey) connectFolder();
    else chooseFile(e.shiftKey);
  }
});
// A press in the view closes the look and section cards and the views menu, as clicking away
// does on the Mac.
canvas.addEventListener("pointerdown", () => {
  closeCards();
  toolbar.viewsMenu.hidden = true;
  toolbar.moreMenu.hidden = true;
});
// The toolbar folds what doesn't fit into More: worked out when the window's width changes, and
// once the fonts its labels are measured in are in. Its room is what viewer.css gives it.
const fitToolbar = () => toolbar.fit(phone || innerWidth <= 900 || !panelOpen ? innerWidth - 16 : innerWidth - 336);
addEventListener("resize", fitToolbar);
document.fonts.ready.then(fitToolbar);
fitToolbar();
chromeReady = true;
applyLook(look, { remember: false });
showEmpty();

// ---- Engine events ----------------------------------------------------------------------------

function handle(kind, document, node, done, total, message) {
  if (!chromeReady) return;
  const event = { kind, document, node, done, total, message };
  switch (kind) {
    case Events.Loading:
      names[document] = message;
      progress.set(document, 0);
      structure.addDocument(message);
      // The left card stays as the person left it: they may be browsing the Library file by file.
      // The structure card is made ready for when they switch to it.
      structure.showNothing(false);
      status.showLoading(message);
      refreshMasthead();
      updateInsets();
      break;
    case Events.Progress:
      // Only while the document is loading: progress arriving after the scene is ready would
      // leave the bar full.
      if (!progress.has(document)) break;
      progress.set(document, total > 0 ? done / total : 0);
      refreshMasthead();
      break;
    case Events.ModelReady:
      structure.modelReady(document);
      status.hide();
      refreshMasthead();
      toolbar.update({ hasSelection: false, hasModel: true });
      rememberOpened(names[document]);
      break;
    case Events.SceneReady:
      progress.clear();
      status.hide();
      refreshMasthead();
      break;
    case Events.Failed:
      progress.clear();
      refreshMasthead();
      status.showFailure(message);
      break;
    case Events.SelectionChanged:
      structure.reveal(document, node);
      refreshSelection();
      break;
    case Events.HoverChanged:
      hovered = node === 0xFFFFFFFF ? null : { document, node, face: done };
      showHover();
      structure.highlight(document, node);
      break;
    case Events.LooksChanged:
      structure.looksChanged();
      break;
    case Events.VisibilityChanged:
      structure.visibilityChanged(done, total);
      break;
  }
  for (const listener of [...listeners]) listener(event);
}

function refreshSelection() {
  const summary = engine.summary();
  selection.show(summary, structure.tree);
  toolbar.update({ hasSelection: summary.occurrences > 0, hasModel: names.length > 0 });
}

function refreshMasthead() {
  if (galleryShown) {
    masthead.show(gallery.title, gallery.subtitle);
    masthead.setProgress(null);
    return;
  }
  if (names.length === 0) {
    masthead.hide();
    return;
  }
  const title = names.length === 1 ? names[0] : `${names.length} files`;
  let parts = 0, bodies = 0;
  for (const d of structure.tree.documents) {
    if (!d.model) continue;
    for (const n of d.model) if (n.kind === NodeKinds.Part) parts += 1;
    bodies += d.model.reduce((most, n) => Math.max(most, n.occurrenceEnd), 0);
  }
  let detail = names.length === 1 ? "" : names.join(", ");
  if (bodies > 0) {
    const count = (n, one, many) => `${n.toLocaleString("en-US")} ${n === 1 ? one : many}`;
    const counts = `${count(parts, "part", "parts")} · ${count(bodies, "body", "bodies")}`;
    detail = detail ? `${counts} · ${detail}` : counts;
  } else {
    detail = "Reading…";
  }
  masthead.show(title, detail);
  const fractions = [...progress.values()];
  masthead.setProgress(fractions.length ? fractions.reduce((a, b) => a + b, 0) / fractions.length : null);
}

function showHover() {
  const tool = canvas.style.cursor === "crosshair";
  if (!hovered || !pointer) {
    hoverChip.show(null);
    if (!tool) canvas.style.cursor = "";
    return;
  }
  hoverChip.show(structure.tree.name(hovered.document, hovered.node), hovered.face, pointer.x, pointer.y);
  if (!tool) canvas.style.cursor = "pointer";
}

// ---- Commands ---------------------------------------------------------------------------------

function run(name) {
  switch (name) {
    case "fit": return engine.perform(Actions.Fit);
    case "projection":
      perspective = !perspective;
      toolbar.projection.selected = perspective ? 1 : 0;
      return engine.perform(Actions.ToggleProjection);
    case "clear":
      // Esc closes what is open first, and only then clears the selection.
      if (!dismissCard()) engine.perform(Actions.ClearSelection);
      return undefined;
    case "isolate": return engine.perform(Actions.Isolate);
    case "showAll": return engine.perform(Actions.ShowAll);
    case "unghostAll": return engine.perform(Actions.UnghostAll);
    case "faces": case "bodies": case "parts": {
      const mode = ["faces", "bodies", "parts"].indexOf(name);
      toolbar.pick.selected = mode;
      engine.setSelectionMode(mode);
      return refreshSelection();
    }
    case "section": return toggleSection();
    case "seeThrough": return engine.perform(Actions.ToggleSeeThrough);
    case "shortcuts":
      shortcuts.element.hidden = !shortcuts.element.hidden;
      return undefined;
    case "undo": return engine.perform(Actions.Undo);
    case "redo": return engine.perform(Actions.Redo);
    default: return undefined;
  }
}

// S cuts across X at the middle, or, with a cut open, removes every cut.
function toggleSection() {
  if (sectionCard.isCutting) {
    sectionCard.cuts.forEach((c) => { c.enabled = false; });
    sectionCard.element.hidden = true;
  } else {
    sectionCard.cuts[0].enabled = true;
    sectionCard.element.hidden = false;
    lookCard.element.hidden = true;
  }
  sectionCard.show();
  engine.setSection(sectionCard.cuts);
}

function toggleCard(card) {
  const other = card === lookCard ? sectionCard : lookCard;
  card.element.hidden = !card.element.hidden;
  other.element.hidden = true;
}

function closeCards() {
  lookCard.element.hidden = true;
  sectionCard.element.hidden = true;
}

// Closes whatever card, menu or sheet is open: the shortcuts sheet, the views and More menus, the
// look and section cards. Returns whether any was.
function dismissCard() {
  const open = [shortcuts.element, toolbar.viewsMenu, toolbar.moreMenu, lookCard.element,
                sectionCard.element].filter((element) => !element.hidden);
  for (const element of open) element.hidden = true;
  return open.length > 0;
}

// Shows `next`, and remembers it unless it is the look the page started with: a look saved only
// once the person changes it keeps following the shared default until then.
function applyLook(next, { remember = true } = {}) {
  look = next;
  if (remember) look.save();
  engine.setLook(look.engineLook);
  const dark = Styles[look.style].onDark;
  masthead.setOnDark(dark);
  document.body.classList.toggle("on-dark", dark);
  gallery?.setStyle(look.engineLook.style);
}

// ---- The gallery --------------------------------------------------------------------------------

// A connected folder's gallery covers the view, the Library beside it; the 3D view draws nothing
// meanwhile. A file opened from it opens in the viewer, and Esc there comes back.
function galleryFor() {
  if (gallery) return gallery;
  // Plates and Sheet ship first; ?views=all shows the views still being built.
  // All five views; Plates and Sheet alone with views=two.
  const views = params.get("views") === "two" ? 0b11 : 0b11111;
  gallery = new Gallery({ engine, store: libraryStore, importers, phone, probes: params.get("test") === "gallery", views }, {
    open: (root, path) => openFromGallery(root, path),
    leave: () => leaveGallery(),
    folderChanged: () => refreshMasthead(),
    changed: () => galleryToolbar.show(gallery.state),
    leavePoster: (toggle = false) => setGalleryPoster(toggle ? !gallery.poster : false),
    // Permission asked again from the lost card's Reconnect, in its click.
    reconnect: async (root) => {
      if (!root || !(await libraryStore.folders.reconnect(root.id))) return;
      await library.refresh();
      await gallery.reconnected();
    },
    search: (query, found) => library.showSearch(galleryShown, query, found),
  });
  canvas.after(gallery.view.element);
  // A press in the gallery closes the cards, as one in the view does.
  gallery.view.element.addEventListener("pointerdown", () => closeCards());
  gallery.setStyle(look.engineLook.style);
  gallery.view.setInsets(galleryInsets());
  return gallery;
}

async function showGallery(root, path) {
  const g = galleryFor();
  if (phone && panelOpen) {
    // Chosen in the Library's sheet: the sheet goes down over the gallery.
    panelOpen = false;
    showLibrary(true);
  }
  enterGallery();
  await g.show(root, path);
}

function enterGallery() {
  galleryShown = true;
  gallery.view.element.hidden = false;
  engine.setCovered(true);
  status.hide();
  toolbar.element.hidden = true;
  galleryToolbar.element.hidden = false;
  galleryToolbar.show(gallery.state);
  library.showSearch(true, gallery.core.search, gallery.core.matches);
  selection.element.hidden = true;
  if (!phone) showLibrary(true);
  gallery.view.reload();
  refreshMasthead();
}

function leaveGallery() {
  dismissCard(); // no card left open over the viewer
  if (gallery.poster) setGalleryPoster(false);
  galleryShown = false;
  gallery.view.element.hidden = true;
  engine.setCovered(false);
  toolbar.element.hidden = false;
  galleryToolbar.element.hidden = true;
  library.showSearch(false);
  if (names.length === 0) showEmpty();
  refreshMasthead();
}

// Show it off: the view as a poster, the full window less 30 each side, the cards put away, with
// Save PNG and Done in the top margin.
function setGalleryPoster(poster) {
  if (!gallery || !galleryShown || phone) return;
  masthead.element.hidden = poster;
  library.element.hidden = poster;
  galleryToolbar.element.hidden = poster;
  galleryToolbar.posterBar.hidden = !poster;
  if (poster) closeCards();
  gallery.view.setInsets(poster ? { top: 30, left: 30, bottom: 30, right: 30 } : galleryInsets());
  gallery.setPoster(poster);
  if (!poster) {
    showLibrary(true);
    refreshMasthead();
  }
}

async function saveGalleryPoster() {
  const png = await gallery?.view.posterPNG();
  if (!png) return;
  const link = el("a", { href: URL.createObjectURL(png), download: `${gallery.title} poster.png` });
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 10_000);
}

async function saveGalleryPDF() {
  if (!gallery) return;
  // A book can take a while: the button counts its pages meanwhile, and takes no second press.
  let pdf;
  try {
    pdf = await gallery.posterPDF((page, pages) => galleryToolbar.savingPDF(`Page ${page} of ${pages}\u2026`));
  } finally {
    galleryToolbar.savingPDF(null);
  }
  if (!pdf) return;
  const what = gallery.core.view === 4 ? "catalogue" : "poster";
  const link = el("a", { href: URL.createObjectURL(pdf), download: `${gallery.title} ${what}.pdf` });
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 10_000);
}

async function openFromGallery(root, path) {
  leaveGallery();
  openedFromGallery = true;
  await openFromFolder({ root: root.id, path, name: path.split("/").pop() }, false);
}

// ---- Layout -----------------------------------------------------------------------------------

// The sidebar put away (open false) or brought back, showing the Library (or the tree). On a
// desktop the page remembers it, and the toolbar, the gallery and framing take the room.
function setPanelOpen(open, library = showingLibrary) {
  panelOpen = open;
  if (!phone) {
    try { localStorage.setItem(kSidebarHidden, open ? "0" : "1"); } catch { /* kept for this visit only */ }
    document.body.classList.toggle("sidebar-hidden", !open);
    fitToolbar();
    if (gallery && !gallery.poster) gallery.view.setInsets(galleryInsets());
  }
  showLibrary(library);
}

// Where a gallery shows: right of the sidebar, or the whole window less the edge once it is away.
function galleryInsets() {
  return phone || panelOpen ? kGalleryInsets : { ...kGalleryInsets, left: kGalleryInsets.right };
}

// The tree card covers the left, the masthead the top and the toolbar the bottom: framing keeps
// the model in the rest.
function updateInsets() {
  const covered = !phone && (!structure.element.hidden || !library.element.hidden);
  insets = { left: covered ? kEdge + kCardWidth : 0, top: kCardTop - 16, right: 0, bottom: kCardBottom - 16 };
  engine.setInsets(insets.left, insets.top, insets.right, insets.bottom);
}

// Nothing open: the invitation, and on a desktop the Library beside it.
function showEmpty() {
  status.showInvitation();
  structure.element.hidden = true;
  library.element.hidden = true;
  libraryEntries().then((kept) => library.refresh(kept)).then(() => {
    if (names.length === 0 && panelOpen) showLibrary(true);
  });
  updateInsets();
  toolbar.update({ hasSelection: false, hasModel: false });
}

// The Library card, or the structure card; the latter says so when nothing is open.
function showLibrary(show) {
  showingLibrary = show;
  library.element.hidden = !show || !panelOpen;
  structure.element.hidden = show || !panelOpen;
  structure.showNothing(names.filter(Boolean).length === 0);
  library.switcher.selected = show ? 0 : 1;
  structure.switcher.selected = show ? 0 : 1;
  if (show) library.refresh();
  updateInsets();
}

// ---- Files ------------------------------------------------------------------------------------

// Where each file shown came from, by name, for Recent: { handle, root, path, where }.
const sources = new Map();

// Opens files from the browser's own picker, which gives handles Recent can read again, or else
// from the page's file input.
async function chooseFile(adding) {
  let picked;
  try {
    picked = await libraryStore.folders.pickFiles();
  } catch (error) {
    status.showFailure(error.message);
    return;
  }
  if (picked === undefined) {
    picker.dataset.adding = adding ? "1" : "0";
    picker.click();
    return;
  }
  if (!picked) return;
  for (const { file, handle } of picked) sources.set(file.name, { handle, where: file.name });
  await openFiles(picked.map((p) => p.file), adding);
}

// Asks for a folder and shows it in the Library, open.
async function connectFolder() {
  if (!libraryStore.canConnect) return;
  try {
    const id = await libraryStore.folders.connect();
    if (id === null || id === undefined) return;
    await showConnected([id]);
  } catch (error) {
    status.showFailure(`That folder could not be connected. ${error.message}`);
  }
}

async function showConnected(ids) {
  persistOnce(); // what is kept of a connected folder's files should outlast storage pressure
  await library.refresh();
  for (const id of ids) library.expand(id);
  // A folder just connected is shown, in the sidebar brought back if it was away.
  if (!phone && !panelOpen) setPanelOpen(true, true);
  showLibrary(true);
}

// A file from a connected folder.
async function openFromFolder(row, add) {
  try {
    const file = await libraryStore.folders.file(row.root, row.path);
    const handle = await libraryStore.folders.fileHandle?.(row.root, row.path).catch(() => null);
    sources.set(file.name, { handle, root: row.root, path: row.path,
                             where: `${library.folderName(row.root)}/${row.path}` });
    await openFiles([file], add);
  } catch (error) {
    status.showFailure(`${row.name} could not be read. ${error.message}`);
  }
}

// A recent file: from where it was, or as it was kept here.
async function reopenRecent(entry, add) {
  const got = await libraryStore.reopen(entry, library.readyFolders).catch(() => null);
  if (!got) {
    status.showFailure(`${entry.name} is no longer where it was.`);
    return;
  }
  sources.set(entry.name, { handle: entry.handle, root: entry.root, path: entry.path, where: entry.where });
  if (got.file) await openFiles([got.file], add);
  else if (got.cache) openCache(got.cache, entry.name, add);
}

// Puts what opened at the top of Recent. Kept imports of files that fall off are deleted, unless
// they are open, so what this browser keeps stays as Recent names.
async function rememberOpened(name) {
  if (!name) return;
  const source = sources.get(name) ?? {};
  const entry = { key: name, name, where: source.where ?? "Kept in this browser", openedAt: Date.now() };
  if (keys.has(name)) entry.cacheKey = keys.get(name);
  for (const field of ["handle", "root", "path"]) if (source[field]) entry[field] = source[field];
  try {
    const dropped = await libraryStore.remember(entry);
    for (const old of dropped) if (!names.includes(old.key)) await forgetKept(old.key);
  } catch {
    // Not remembered: the file is open all the same.
  }
  library.setOpen(openKeys());
  await library.refresh(); // shows it in Recent at once, as the Mac does
}

async function forgetRecent(key) {
  const entry = (await libraryStore.recent()).find((r) => r.key === key);
  await libraryStore.forget(key);
  if (!names.includes(key)) {
    await forgetKept(key);
    if (entry?.cacheKey) await forgetKeptKey(entry.cacheKey);
  }
}

// The Library's keys of what is shown: names, and paths in connected folders.
function openKeys() {
  const keys = [];
  for (const name of names.filter(Boolean)) {
    keys.push(name);
    const source = sources.get(name);
    if (source?.root) keys.push(keyOf(source.root, source.path));
  }
  return keys;
}

function forget() {
  names = [];
  progress.clear();
  structure.reset();
  selection.element.hidden = true;
  library.setOpen([]);
  // The next file starts without a cut, as the engine does: the cuts go off, keeping where they
  // were placed.
  sectionCard.cuts.forEach((cut) => { cut.enabled = false; });
  sectionCard.show();
  sectionCard.element.hidden = true;
}

// Closes everything open: back to the Library and the invitation to open something.
function closeFiles() {
  forget();
  closeCards();
  engine.close();
  refreshMasthead();
  showEmpty();
}

function openCache(source, name, add) {
  if (!add) forget();
  engine.open(source, name, { add });
}

async function openFiles(files, adding) {
  for (const [index, file] of files.entries()) {
    const add = adding || index > 0;
    if (/\.geoblob$/i.test(file.name)) {
      openCache(file, file.name, add);
    } else if (/\.(step|stp|stl)$/i.test(file.name)) {
      await importFile(file, add);
    } else {
      status.showFailure(`${file.name} is not a STEP, STL or cache file.`);
    }
  }
}

// ---- Importing --------------------------------------------------------------------------------

// The importer runs in a worker, with its own pool of OCCT workers, one file at a time. Phones
// get two OCCT workers, as P2 measured best; desktops one per core, less two for the page. What is
// opened goes through the importers' live lane: one at a time, in the order asked, and a file being
// opened cancels a refinement running; the gallery's imports have a lane of their own.
let importer = null;
const importers = new Importers({ shared: phone });
let importing = Promise.resolve(); // the latest file asked for, for the scripted checks
let nextJob = 1;
const stepSources = new Map(); // STEP files imported this visit, by name, for refinement
const keys = new Map(); // names of files imported this visit, to their kept imports' keys
const importWorkers = phone ? 2 : Math.max(2, Math.min(12, (navigator.hardwareConcurrency ?? 4) - 2));

// Where a file came from, for its kept import's key: a connected folder's name and its path there,
// or "file" and its name for one opened on its own.
function originOf(file) {
  const source = sources.get(file.name);
  return source?.root ? { source: library.folderName(source.root) || source.root, path: source.path }
                      : { source: "file", path: file.name };
}

async function keyFor(file) {
  return keptKeyOf({ ...originOf(file), size: file.size, modified: file.lastModified, version: engine.cacheVersion });
}

// Keeps what is kept within its budget, sparing what is open, after an import adds to it.
function evictKept() {
  const open = new Set(names.map((name) => keys.get(name)).filter(Boolean));
  evictImports(open).catch(() => {});
}

// What importing an STL file takes at its peak. A binary file, read in pieces, is never held
// whole, and the importer's heap stays under 300 MB and 115 bytes a triangle: 0.46 GB for 2
// million, 0.87 GB for 5 and 1.27-1.37 GB for 10. Any other is read whole, and is reckoned as it
// was measured before: 250 bytes a triangle, with the file. A phone stops a page well before 2 GB,
// so there a file that would pass about 1.2 GB is refused, with a way forward, rather than lost
// midway.
function stlImportBytes(file) {
  const triangles = (file.size - 84) / 50;
  if (Number.isInteger(triangles) && triangles > 0) return 300 * 1024 ** 2 + triangles * 115;
  return file.size + Math.max(0, triangles) * 250;
}
const kPhoneImportBudget = 1.2 * 1024 ** 3;

function importFile(file, add) {
  if (!/\.stl$/i.test(file.name)) stepSources.set(file.name, file);
  importing = importers.live(`open-${nextJob++}`, async (signal) => {
    const key = await keyFor(file);
    keys.set(file.name, key);
    // Kept before by key, or by name before keys were kept.
    const cached = (await keptImport(key, engine.cacheVersion))?.blob ?? await cachedImport(file);
    if (cached) {
      openCache(cached, file.name, add);
      return { type: "cached" };
    }
    if (phone && /\.stl$/i.test(file.name) && stlImportBytes(file) > kPhoneImportBudget) {
      const message = `${file.name} is too large to import on a phone. Import it on a computer, ` +
                      "then open its cache file here.";
      status.showFailure(message);
      return { type: "failed", message };
    }
    // STEP files go to the long-lived importer, whose OCCT pool stays warm; an STL file to one
    // of its own, ended afterwards, which frees its threads and memory.
    const stl = /\.stl$/i.test(file.name);
    const worker = stl ? new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" })
                       : (importer ??= new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" }));
    // A STEP file's document opens at once and fills as its segments are written, as on the Mac;
    // an STL file's single segment comes at the end, so it opens from the library when done.
    let token = null, document = -1, shown = false;
    if (!stl) {
      if (!add) forget();
      token = await engine.openImport(file.name, { add });
      document = names.length - 1;
    }
    status.showImporting(file.name, 0, 0);
    const result = await new Promise((resolve) => {
      worker.onmessage = ({ data }) => {
        if (data.type === "segment") {
          if (token !== null && engine.importSegment(token, data.bytes) && !shown) {
            shown = true; // something to look at: the progress moves to the masthead
            status.hide();
          }
        } else if (data.type === "progress") {
          if (shown) {
            if (progress.has(document)) progress.set(document, data.total > 0 ? data.done / data.total : 0);
            refreshMasthead();
          } else {
            status.showImporting(file.name, data.done, data.total);
          }
        } else {
          resolve(data);
        }
      };
      worker.onerror = (event) => resolve({ type: "failed", message: event.message ?? "The importer failed." });
      signal.addEventListener("abort", () => worker.postMessage({ type: "cancel" }));
      // A file in a connected folder can be read from there again, so its source is not kept.
      const fromFolder = Boolean(sources.get(file.name)?.root);
      worker.postMessage({ type: "import", file, kind: stl ? "stl" : "step", name: file.name, key,
                           keepSource: !stl && !fromFolder, workers: importWorkers, threads: stl,
                           progressive: !stl });
    });
    if (stl) worker.terminate();
    lastImport = result;
    if (result.type !== "done") {
      if (token !== null) engine.finishImport(token, result.message);
      else status.showFailure(result.message);
      return result;
    }
    if (token !== null) engine.finishImport(token);
    else openCache((await keptImport(key))?.blob, file.name, add);
    evictKept();
    return result;
  });
  return importing;
}

// ---- Refinement --------------------------------------------------------------------------------

// The engine asks for finer geometry where the settled view is close, as on the Mac: the STEP
// file's definitions it names are imported again at a finer level, and their segments join the
// document. The file is the one imported this visit, the one in its connected folder, or the one
// the library kept beside its cache. Refinements wait their turn behind files being opened, in the
// same worker and its warm OCCT pool, and one running is cancelled for a file being opened.
let refinement = null;

// A STEP file's source again, for refining: from this visit, its folder, or what was kept.
async function sourceFor(name) {
  const visit = stepSources.get(name);
  if (visit) return visit;
  const where = sources.get(name);
  if (where?.root && library.readyFolders.has(where.root)) {
    const file = await libraryStore.folders.file(where.root, where.path).catch(() => null);
    if (file) return file;
  }
  const key = keys.get(name);
  return (key ? await keptSourceOf(key) : null) ?? await keptSource(name);
}

function refineDocument(token, name, level, definitions) {
  const job = { cancelled: false, id: `refine-${nextJob++}` };
  refinement = job;
  const asked = performance.now();
  importers.live(job.id, async (signal) => {
    let ok = false;
    const started = performance.now();
    let note = "";
    let made = null, segments = 0, refused = 0;
    signal.addEventListener("abort", () => {
      job.cancelled = true;
      importer?.postMessage({ type: "cancel" });
    });
    try {
      const file = job.cancelled ? null : await sourceFor(name);
      if (!file || job.cancelled) return;
      importer ??= new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" });
      const result = await new Promise((resolve) => {
        importer.onmessage = ({ data }) => {
          if (data.type === "segment") {
            segments += 1;
            if (!job.cancelled && !engine.refineSegment(token, data.bytes)) refused += 1;
          } else if (data.type !== "progress") {
            resolve(data);
          }
        };
        importer.onerror = (event) => resolve({ type: "failed", message: event.message });
        importer.postMessage({ type: "refine", file, level, definitions, workers: importWorkers });
      });
      ok = result.type === "done";
      note = result.message ?? "";
      if (ok) {
        made = { triangles: result.triangles, failedFaces: result.failedFaces,
                 coarserFaces: result.coarserFaces, crashedWorkers: result.crashedWorkers };
      }
      // The view keeps the geometry it has, so a failure is only worth a note for diagnosing.
      if (!ok && !job.cancelled) console.warn(`Refining ${name} to level ${level} failed: ${result.message}`);
    } finally {
      refinements.push({ name, level, definitions: definitions.length, waitedMs: Math.round(started - asked),
                         ms: Math.round(performance.now() - started), ok, cancelled: job.cancelled, note,
                         made, segments, refused });
      if (!job.cancelled) engine.finishRefinement(token, ok);
      if (refinement === job) refinement = null;
    }
  }, { band: "visible" });
}

// Each refinement job, as the scripted check reports them.
const refinements = [];

function cancelRefinement() {
  if (!refinement) return;
  refinement.cancelled = true;
  importers.cancel(refinement.id);
}

let lastImport = null;
let importStarted = 0;

// Removes what was kept of a file by its name, so the next open imports again.
async function forgetImport(name) {
  await forgetKept(name);
  await forgetNamed(name);
}

// Dropping files: holding ⌘ adds them to what is shown.
let dragDepth = 0;
addEventListener("dragenter", (e) => {
  if (![...e.dataTransfer.types].includes("Files")) return;
  e.preventDefault();
  dragDepth += 1;
  dropZone.element.hidden = false;
});
addEventListener("dragover", (e) => e.preventDefault());
addEventListener("dragleave", () => {
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0) dropZone.element.hidden = true;
});
// Files open; folders connect. The drop's items are read at once, as browsers require.
addEventListener("drop", async (e) => {
  e.preventDefault();
  dragDepth = 0;
  dropZone.element.hidden = true;
  const add = e.metaKey || e.ctrlKey;
  const { files, connected } = await libraryStore.adopt(e.dataTransfer);
  if (connected.length > 0) await showConnected(connected);
  for (const { file, handle } of files) if (handle) sources.set(file.name, { handle, where: file.name });
  if (files.length > 0) await openFiles(files.map((f) => f.file), add);
});

// The fixtures are beside the page when served from the build, and in the spike's folder in the
// iOS shell, which passes fixtures=/spike/fixtures/.
const model = params.get("model") ?? params.get("import");
const fixtures = params.get("fixtures") ?? "fixtures/";
if (params.has("import")) {
  // A fixture imported as a dropped file would be: import=step/ple120.step.
  const path = params.get("import");
  const response = await fetch(`${fixtures}${path}`);
  const file = new File([await response.blob()], path.split("/").pop(), { lastModified: 1 });
  if (params.has("fresh")) await forgetImport(file.name);
  importStarted = performance.now();
  importFile(file, false);
} else if (model) {
  openCache(`${fixtures}${model}.geoblob`, `${model}.geoblob`, false);
}

// ---- Samples ---------------------------------------------------------------------------------

// A hosted copy carries models to try, listed in samples/index.json beside the page. Served by the
// launcher or the iOS shell there are none, and nothing is offered.
const samples = fetch("samples/index.json")
  .then((response) => (response.ok ? response.json() : { samples: [] }))
  .then((index) => index.samples ?? [], () => []);
samples.then((list) => status.setSamples(list, (sample) => openSample(sample)));

// Downloads a sample, saying how far it has got, then opens it as a file the person chose.
async function openSample(sample) {
  try {
    const response = await fetch(`samples/${sample.file}`);
    if (!response.ok) throw new Error(`the server answered ${response.status}`);
    // The index's size, since a compressed response's length is not the file's.
    const total = sample.size || Number(response.headers.get("Content-Length"));
    const chunks = [];
    let done = 0, shown = 0;
    for (const reader = response.body.getReader(); ;) {
      const { value, done: finished } = await reader.read();
      if (finished) break;
      chunks.push(value);
      done += value.byteLength;
      if (performance.now() - shown > 100) {
        status.showDownloading(sample.file, done, total);
        shown = performance.now();
      }
    }
    await importFile(new File(chunks, sample.file, { lastModified: 1 }), false);
  } catch (error) {
    status.showFailure(`${sample.name} could not be downloaded: ${error.message}.`);
  }
}

if (params.has("sample")) {
  const sample = (await samples).find((each) => each.id === params.get("sample"));
  if (sample) openSample(sample);
}

// In the iOS shell: documents the person opens from Files or the share sheet arrive here.
window.geometryViewShell = {
  async open(url, name, lastModified) {
    if (/\.geoblob$/i.test(name)) {
      openCache(url, name, false);
      return;
    }
    status.showLoading(name);
    const response = await fetch(url);
    if (!response.ok) {
      status.showFailure(`${name} could not be read.`);
      return;
    }
    await importFile(new File([await response.blob()], name, { lastModified }), false);
  },
};
window.webkit?.messageHandlers?.shell?.postMessage({ ready: true });

// Opened by the launcher: take the AI app's requests.
if (params.has("token")) {
  const { connectBridge } = await import("./bridge.js");
  connectBridge(params.get("token"), bridgeHandlers());
}

if (params.get("test") === "import") runImportTest().catch((error) => post({ error: error.message }));
else if (params.get("test") === "library") runLibraryTest().catch((error) => post({ error: error.message }));
else if (params.get("test") === "refine") runRefineTest().catch((error) => post({ error: error.message }));
else if (params.get("test") === "thumbs") runThumbsTest().catch((error) => post({ error: error.message }));
else if (params.get("test") === "worker") runWorkerTest().catch((error) => post({ error: error.message }));
else if (params.get("test") === "gallery") runGalleryTest().catch((error) => post({ error: error.message, stack: error.stack }));
else if (params.has("test")) runTest().catch((error) => post({ error: error.message }));

// ---- The launcher's bridge ---------------------------------------------------------------------

// What the AI app's tools ask of the page, when the launcher opened it. Each answers with plain
// data; a picture comes back as a PNG in base64.
function bridgeHandlers() {
  const kinds = ["assembly", "part", "body"];
  let isolatedByRequest = false;

  const describe = () => {
    const info = engine.sceneInfo();
    let assemblies = 0, parts = 0, bodies = 0;
    for (const d of structure.tree.documents) {
      if (!d.model) continue;
      for (const n of d.model) {
        if (n.kind === NodeKinds.Assembly) assemblies += 1;
        else if (n.kind === NodeKinds.Part) parts += 1;
      }
      bodies += d.model.reduce((most, n) => Math.max(most, n.occurrenceEnd), 0);
    }
    return { names: names.filter(Boolean), assemblies, parts, bodies, faces: info.faces,
             triangles: info.triangles, min: info.min, max: info.max };
  };

  // A style and colours, as the look card numbers them; either may be left as it is.
  const restyle = (style, colour) => {
    if (style === undefined && colour === undefined) return;
    const next = Object.assign(new Look(), look);
    if (style !== undefined) next.choose(style);
    if (colour !== undefined) next.colour = colour;
    look = next;
    lookCard.show(next);
    engine.setLook(next.engineLook);
  };

  // Shows alone the parts and assemblies whose names contain one of `isolate`, framed, after
  // undoing what an earlier request isolated. Returns how many it found. Only the outermost of
  // nested matches is selected: "rotor" finds the rotor assembly and its rotor_core, and selecting
  // the core again, inside the selected assembly, would take it out.
  const showAlone = (isolate) => {
    if (isolate.length > 0 || isolatedByRequest) engine.perform(Actions.ShowAll);
    isolatedByRequest = false;
    if (isolate.length === 0) return 0;
    const wanted = isolate.map((n) => n.toLowerCase());
    engine.perform(Actions.ClearSelection);
    let found = 0;
    structure.tree.documents.forEach((d, document) => {
      let insideMatch = 0; // nodes before this one are inside the last match
      d.model?.forEach((n, node) => {
        if (node < insideMatch || n.kind === NodeKinds.Body) return;
        if (wanted.some((w) => n.name.toLowerCase().includes(w))) {
          engine.selectNode(document, node, true);
          found += 1;
          insideMatch = n.end;
        }
      });
    });
    if (found === 0) throw new Error(`No part's name contains ${isolate.join(" or ")}.`);
    engine.perform(Actions.Isolate);
    engine.perform(Actions.ClearSelection);
    isolatedByRequest = true;
    return found;
  };

  const toBase64 = (bytes) => {
    let text = "";
    for (let at = 0; at < bytes.length; at += 0x8000) text += String.fromCharCode(...bytes.subarray(at, at + 0x8000));
    return btoa(text);
  };

  return {
    async open({ url, name, add = false, lastModified }) {
      const ready = waitFor(Events.SceneReady);
      if (/\.geoblob$/i.test(name)) {
        openCache(url, name, add);
      } else {
        const response = await fetch(url);
        if (!response.ok) throw new Error(`The launcher could not serve ${name}.`);
        const file = new File([await response.blob()], name, { lastModified: lastModified ?? Date.now() });
        const result = await importFile(file, add);
        if (result.type === "failed") throw new Error(result.message);
      }
      const event = await ready;
      if (event.kind === Events.Failed) throw new Error(event.message);
      isolatedByRequest = false;
      return describe();
    },

    async describe() {
      if (names.length === 0) throw new Error("Nothing is open in the viewer.");
      return describe();
    },

    async structure({ document = 0 }) {
      const model = engine.model(document);
      if (!model) throw new Error("Nothing is open in the viewer.");
      return model.map((n) => ({ parent: n.parent, kind: kinds[n.kind] ?? "body", name: n.name,
                                 bodies: [n.firstOccurrence, n.occurrenceEnd] }));
    },

    // A picture as the AI app asked: a direction, a style and colours, parts shown alone, and
    // cuts. The viewer shows it too, so the person sees what the AI looked at.
    // Some parts shown alone, and a style and colours, as the AI app asked: the view stays the
    // person's to turn.
    async show({ style, colour, isolate = [] }) {
      if (names.length === 0) throw new Error("Nothing is open in the viewer.");
      restyle(style, colour);
      return { ...describe(), isolated: showAlone(isolate) };
    },

    async render({ azimuth = 35, elevation = 25, perspective: persp = false, style, colour,
                   isolate = [], section = [], width = 1200, height = 800 }) {
      if (names.length === 0) throw new Error("Nothing is open in the viewer.");
      restyle(style, colour);

      sectionCard.cuts.forEach((cut) => { cut.enabled = false; });
      for (const cut of section) {
        const axis = "xyz".indexOf(cut.axis);
        if (axis >= 0) Object.assign(sectionCard.cuts[axis], { enabled: true, position: cut.at, flip: Boolean(cut.flip) });
      }
      sectionCard.show();
      engine.setSection(sectionCard.cuts);

      showAlone(isolate);

      perspective = persp;
      toolbar.projection.selected = persp ? 1 : 0;
      engine.setView(azimuth, elevation, persp);
      const image = await engine.snapshot();
      if (!image) {
        const reason = engine.lastError();
        throw new Error(`The viewer could not draw a picture.${reason ? ` ${reason}` : ""}`);
      }

      // The part of the view the cards leave clear, fitted into the size asked for.
      const scale = engine.scale;
      const x = Math.round(insets.left * scale), y = Math.round(insets.top * scale);
      const w = image.width - x - Math.round(insets.right * scale);
      const h = image.height - y - Math.round(insets.bottom * scale);
      const whole = new OffscreenCanvas(image.width, image.height);
      whole.getContext("2d").putImageData(
          new ImageData(new Uint8ClampedArray(image.pixels.buffer), image.width, image.height), 0, 0);
      const picture = new OffscreenCanvas(width, height);
      const context = picture.getContext("2d");
      // Letterboxed with the picture's own background, from its corner.
      const corner = (y * image.width + x) * 4;
      context.fillStyle = `rgb(${image.pixels[corner]}, ${image.pixels[corner + 1]}, ${image.pixels[corner + 2]})`;
      context.fillRect(0, 0, width, height);
      const fit = Math.min(width / w, height / h);
      context.drawImage(whole, x, y, w, h, (width - w * fit) / 2, (height - h * fit) / 2, w * fit, h * fit);
      const png = new Uint8Array(await (await picture.convertToBlob({ type: "image/png" })).arrayBuffer());
      return { png: toBase64(png), width, height };
    },
  };
}

// ---- Scripted checks --------------------------------------------------------------------------

function testLabel() {
  // A report's name is a file's, so an imported fixture's folder joins it with a dash.
  const name = (model ?? params.get("file") ?? (params.get("test") === "gallery" ? params.get("folder") : null) ?? "none")
    .replaceAll("/", "-");
  // The gallery's checks are reported with its measurements.
  const phase = ["thumbs", "worker"].includes(params.get("test")) ? "g1"
    : params.get("test") === "gallery" ? "g9" : "p4";
  return `${params.get("label") ?? "unlabelled"}-${phase}-${params.get("test")}-${name}`;
}

async function post(report) {
  const body = JSON.stringify({ label: testLabel(), userAgent: navigator.userAgent, devicePixelRatio,
                                errors, ...report });
  const shell = window.webkit?.messageHandlers?.census;
  if (shell) shell.postMessage(body);
  else await fetch("/report", { method: "POST", body }).catch(() => {});
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The view, as a PNG the server keeps beside the fixtures.
async function postSnapshot() {
  const image = await engine.snapshot();
  if (!image || window.webkit?.messageHandlers?.census) return;
  const picture = new OffscreenCanvas(image.width, image.height);
  picture.getContext("2d").putImageData(
      new ImageData(new Uint8ClampedArray(image.pixels.buffer), image.width, image.height), 0, 0);
  const png = await picture.convertToBlob({ type: "image/png" });
  await fetch(`/png?label=${encodeURIComponent(testLabel())}`, { method: "POST", body: png }).catch(() => {});
}

// Imports a fixture, opens what it made, and reports how long each took and what was made.
async function runImportTest() {
  // An import the page refuses, or that fails in its worker, never reaches the engine.
  const refused = importing.then((result) => result?.type === "failed" ? result : new Promise(() => {}));
  const ready = await Promise.race([waitFor(Events.SceneReady), refused]);
  if (ready.type === "failed") {
    await post({ failed: ready.message, workers: importWorkers });
    return;
  }
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  const totalMs = performance.now() - importStarted;
  await sleep(1500);
  const stats = engine.stats();
  if (!params.has("nosnapshot")) await postSnapshot();
  await post({ totalMs: Math.round(totalMs), import: lastImport, cached: lastImport === null,
               workers: importWorkers, triangles: stats.triangles, bufferMB: stats.bufferMB,
               heapMB: stats.heapMB });
}

// Connects a folder, made in the origin-private file system as a person's would be on a disk (in
// the iPhone shell, the shell connects one in its own storage), opens a file from it, then the
// same file from Recent, which should come from the kept import, and counts the frames drawn while
// nothing happens with the Library showing. Reports each step. With keep, what an earlier run kept
// is left, so the first open shows whether it lasted.
async function runLibraryTest() {
  const name = params.get("file") ?? "pack.step";
  if (!params.has("keep")) await forgetImport(name);
  const keptBefore = (await libraryEntries()).map((e) => e.name);
  let id;
  if (libraryStore.kind === "shell") {
    id = (await libraryStore.folders.folders()).find((f) => f.name === "library-test")?.id;
    if (!id) throw new Error("The shell connected no test folder.");
  } else {
    const bytes = await (await fetch(`${fixtures}step/${name}`)).blob();
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry("library-test", { recursive: true }).catch(() => {});
    const folder = await opfs.getDirectoryHandle("library-test", { create: true });
    const inner = await folder.getDirectoryHandle("sub", { create: true });
    const writable = await (await inner.getFileHandle(name, { create: true })).createWritable();
    await writable.write(bytes);
    await writable.close();
    await (await (await folder.getFileHandle("notes.txt", { create: true })).createWritable()).close();
    id = libraryStore.kind === "visit"
      ? libraryStore.folders.adoptFiles("library-test", new Map([
          [`sub/${name}`, new File([bytes], name, { lastModified: 1 })], ["notes.txt", new File([], "notes.txt")]]))
      : await libraryStore.folders.adoptHandle(folder);
  }
  await showConnected([id]);
  const listed = (await libraryStore.folders.list(id, "")).map((e) => e.name).sort();
  // A gallery's walk: its folders and model files, by path.
  const walked = [];
  for await (const entry of libraryStore.folders.walk(id)) walked.push(`${entry.kind}:${entry.path}`);

  lastImport = null;
  const opened = performance.now();
  let ready = waitFor(Events.SceneReady);
  await openFromFolder({ root: id, path: `sub/${name}`, name }, false);
  await ready;
  const firstMs = Math.round(performance.now() - opened);
  const imported = lastImport?.type === "done";
  // Opening a file leaves the Library showing: the card switches only when the person switches it.
  const stayedOnLibrary = !library.element.hidden || phone;
  await sleep(300);
  const recent = await libraryStore.recent();

  lastImport = null;
  const again = performance.now();
  ready = waitFor(Events.SceneReady);
  await reopenRecent(recent[0], false);
  await ready;
  const reopenMs = Math.round(performance.now() - again);
  const reopenedFromCache = lastImport === null;

  showLibrary(true);
  await sleep(1500);
  const settled = engine.stats();
  await sleep(3000);
  const idle = engine.stats();
  const rows = [...library.element.querySelectorAll(".row")].map((r) => r.textContent.trim());

  if (!params.has("keep")) {
    await libraryStore.folders.disconnect(id);
    await forgetRecent(name);
    const opfs = await navigator.storage.getDirectory();
    await opfs.removeEntry("library-test", { recursive: true }).catch(() => {});
  }
  await post({ backend: libraryStore.kind, origin: location.origin, keptBefore, listed, walked, firstMs, imported,
               stayedOnLibrary, stillOnLibrary: !library.element.hidden || phone,
               recentFirst: recent[0]?.key,
               recentWhere: recent[0]?.where, reopenMs, reopenedFromCache,
               idleFrames: idle.frames - settled.frames, looping: idle.looping, rows });
}

// Draws fixtures' pictures in the gallery's picture worker, one after another as the gallery
// will, and reports how long each took, the most memory the worker held and how often it was
// replaced; the first eight go on a sheet, kept as a PNG.
async function runThumbsTest() {
  const { Thumbnailer } = await import("./thumbs.js");
  const files = (params.get("files") ?? "gallery-drive,gallery-motor").split(",");
  const count = Number(params.get("count") ?? 40);
  const thumbnailer = new Thumbnailer({ recycleMB: Number(params.get("recycle") ?? 256) });
  const blobs = await Promise.all(files.map(async (f) => (await fetch(`${fixtures}${f}.geoblob`)).blob()));
  const spec = { pixels: Number(params.get("size") ?? 256), style: Number(params.get("style") ?? 0),
                 colorMode: 1, pixelsPerPoint: 2, lineScale: 0.5 };
  const times = [];
  let firstMs = 0, maxHeapMB = 0, firstPicture = null;
  const kept = [];
  for (let i = 0; i < count; i++) {
    const started = performance.now();
    const picture = await thumbnailer.draw(blobs[i % blobs.length], spec, i % 4);
    const ms = performance.now() - started;
    if (i === 0) firstMs = ms;
    else times.push(ms);
    maxHeapMB = Math.max(maxHeapMB, thumbnailer.heapMB);
    firstPicture ??= { width: picture.width, partRect: picture.partRect, inkRect: picture.inkRect,
                       worldPerPixel: picture.worldPerPixel, pngBytes: picture.png.size };
    if (kept.length < 8) kept.push(picture.bitmap);
    else picture.bitmap.close();
  }
  thumbnailer.close();
  const sheet = new OffscreenCanvas(spec.pixels * kept.length, spec.pixels);
  const context = sheet.getContext("2d");
  context.fillStyle = "#f2ead8";
  context.fillRect(0, 0, sheet.width, sheet.height);
  kept.forEach((bitmap, i) => context.drawImage(bitmap, i * spec.pixels, 0));
  const png = await sheet.convertToBlob({ type: "image/png" });
  await fetch(`/png?label=${encodeURIComponent(testLabel())}`, { method: "POST", body: png }).catch(() => {});
  times.sort((a, b) => a - b);
  const at = (q) => Math.round(times[Math.min(times.length - 1, Math.floor(q * times.length))] * 100) / 100;
  await post({ files, count, size: spec.pixels, style: spec.style, firstMs: Math.round(firstMs),
               p50Ms: at(0.5), p90Ms: at(0.9), maxHeapMB, recycled: thumbnailer.recycled, firstPicture });
}

// The import worker's gallery messages: a STEP file's structure alone, as one model segment; and
// an import cancelled soon after it starts, which stops with its slices out.
async function runWorkerTest() {
  const name = params.get("file") ?? "pack.step";
  const file = new File([await (await fetch(`${fixtures}step/${name}`)).blob()], name);
  const worker = new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" });
  const ask = (message, onSegment = () => {}) => new Promise((resolve) => {
    worker.onmessage = ({ data }) => {
      if (data.type === "segment") onSegment(data.bytes);
      else if (data.type !== "progress") resolve(data);
    };
    worker.postMessage(message);
  });
  let started = performance.now();
  const segments = [];
  const structure = await ask({ type: "structure", file }, (bytes) => segments.push(bytes.length));
  const structureMs = Math.round(performance.now() - started);

  started = performance.now();
  let progressed = false;
  const cancelled = new Promise((resolve) => {
    worker.onmessage = ({ data }) => {
      if (data.type === "progress" && data.done > 0 && !progressed) {
        progressed = true;
        worker.postMessage({ type: "cancel" });
      } else if (data.type === "done" || data.type === "failed") {
        resolve(data);
      }
    };
  });
  worker.postMessage({ type: "import", file, kind: "step", name, key: "worker-test", keepSource: false,
                       workers: importWorkers, threads: false, progressive: false });
  const outcome = await cancelled;
  const cancelMs = Math.round(performance.now() - started);
  worker.terminate();
  await forgetKeptKey("worker-test");
  await post({ file: name, structure: structure.type, structureMs, structureSegments: segments,
               cancelOutcome: outcome.type, cancelled: outcome.cancelled ?? false, cancelMs,
               message: outcome.message ?? "" });
}

// A served folder's gallery (?folder=test-models), measured as the Mac's --gallery-check measures
// its own: in Illustration, Blueprint and Studio, the folder and its first subfolders, every
// alignment group as painted, and how far anything was painted from where the core laid it. Then
// the view is scrolled through its tallest folder while long tasks are counted, and the 3D view's
// frames while the gallery covers it.
async function runGalleryTest() {
  const dir = params.get("folder") ?? "test-models";
  const name = dir.split("/").pop();
  let id, listing;
  if (libraryStore.kind === "shell") {
    // On the iPhone: the folder the shell connected from its own storage, walked as the gallery
    // walks it.
    id = (await libraryStore.folders.folders()).find((f) => f.name === name)?.id;
    if (!id) throw new Error(`The shell connected no folder named ${name}.`);
    listing = [];
    for await (const entry of libraryStore.folders.walk(id)) listing.push({ path: entry.path, folder: entry.kind === "folder" });
  } else {
    listing = await (await fetch(`/list?dir=${encodeURIComponent(dir)}`)).json();
    if (!libraryStore.folders.adoptRemote) throw new Error("Run with backend=visit.");
    id = libraryStore.folders.adoptRemote(name, listing, `/viewer/${dir}/`);
  }
  await library.refresh();
  const root = { id, name };
  const g = galleryFor();
  await showGallery(root, "");
  const settle = async () => {
    const started = performance.now();
    while (g.waitingInView > 0 && performance.now() - started < Number(params.get("wait") ?? 240000)) await sleep(250);
    await sleep(400);
    g.view.repaint();
    await new Promise(requestAnimationFrame);
    return Math.round(performance.now() - started) / 1000;
  };
  const subfolders = [...new Set(listing.filter((e) => e.folder && !e.path.includes("/")).map((e) => e.path))]
    .filter((p) => listing.some((e) => !e.folder && e.path.startsWith(`${p}/`))).sort().slice(0, 3);
  const runs = [];
  let worst = 0, over = 0, deviation = 0;
  // Every alignment group as painted, and how far each item was painted from where the core laid it.
  const measure = () => {
    const { lines, columns } = g.view.measured();
    const result = g.core.check(lines, columns, 0.5);
    const items = g.core.items;
    let off = 0, worstItem = null;
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      const d = Math.max(Number.isNaN(lines[i]) ? 0 : Math.abs(lines[i] - it.line),
                         it.column && !Number.isNaN(columns[i]) ? Math.abs(columns[i] - it.columnLine) : 0);
      if (d > off) {
        off = d;
        // The item furthest off, to look into: what it is, where laid and where painted.
        worstItem = { kind: it.kind, flags: it.flags, x: it.x, y: it.y, width: it.width, height: it.height,
                      line: it.line, painted: lines[i], column: it.columnLine, paintedColumn: columns[i],
                      lineWidth: it.lineWidth, fontSize: it.fontSize, group: it.group };
      }
    }
    deviation = Math.max(deviation, off);
    worst = Math.max(worst, ...result.over.map((o) => o.spread), 0);
    over += result.over.length;
    // The toolbar fits its room: folded as it must be, never cut off.
    const bar = galleryToolbar.element;
    return { groupsChecked: result.checked, over: result.over, painted: lines.filter((v) => !Number.isNaN(v)).length,
             maxDeviation: Math.round(off * 1000) / 1000, ...(off > 0.1 ? { worstItem } : {}), toolbarCut: Math.max(0, bar.scrollWidth - bar.clientWidth),
             toolbarFold: bar.classList.contains("tighter") ? 2 : bar.classList.contains("tight") ? 1 : 0 };
  };
  // checkViews (a list, "4" or "0,4") runs only those, for looking into one.
  const views = params.has("checkViews") ? params.get("checkViews").split(",").map(Number)
    : params.get("views") === "all" ? [0, 1, 2, 3, 4] : [0, 1];
  // scrollOnly skips the styles' runs, for timing a scroll alone; styles=0 runs Illustration only,
  // to keep a phone's run short.
  const styles = params.has("styles") ? params.get("styles").split(",").map(Number) : [0, 3, 5];
  for (const style of params.has("scrollOnly") ? [] : styles) {
    const next = Object.assign(Object.create(Object.getPrototypeOf(look)), look);
    next.choose(style);
    applyLook(next, { remember: false });
    for (const view of views) {
      g.setView(view);
      for (const path of ["", ...subfolders]) {
        await g.show(root, path);
        g.setView(view);
        await sleep(600); // past any move
        const waited = await settle();
        if (params.has("shots") && path === "") {
          // Whoever runs the browser takes a screenshot while the page waits.
          await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-${dir.replaceAll("/", "-")}-${["plates", "sheet", "ladders", "atlas", "catalogue"][view]}-${style}`)}`);
          await sleep(1500);
        }
        runs.push({ style, view, folder: path, waited, ...measure() });
        if (view === 4) {
          // Catalogue's other spreads: the contents, a plate and its table, the index.
          const { spreads } = g.core.catalogue;
          for (const spread of [...new Set([1, 2, 3, Math.floor(spreads / 2), spreads - 1])].filter((s) => s > 0 && s < spreads)) {
            g.catalogue({ spread });
            await sleep(200);
            await settle();
            if (params.has("shots") && path === "") {
              await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-${dir.replaceAll("/", "-")}-catalogue-${style}-s${spread}`)}`);
              await sleep(1500);
            }
            runs.push({ style, view, spread, folder: path, waited: 0, ...measure() });
          }
        }
        if (view === 3) {
          // Atlas closer in: the districts, then every part named.
          for (const level of [1, 2]) {
            g.atlas({ level });
            await sleep(300);
            await settle();
            runs.push({ style, view, level, folder: path, waited: 0, ...measure() });
          }
        }
      }
    }
  }
  if (params.has("lost")) {
    // The folder drawn, then unreadable, as when the browser's permission lapses: shown again from
    // what was kept, faded, with Reconnect; then readable again.
    g.setView(0);
    await g.show(root, "");
    await settle();
    await sleep(2500); // past the listing being kept
    const folders = libraryStore.folders;
    const { walk, file } = folders;
    const denied = () => { throw new DOMException("The folder cannot be read.", "NotAllowedError"); };
    folders.walk = async function* () { denied(); };
    folders.file = async () => denied();
    g.root = null;
    await g.show(root, "");
    await sleep(3000);
    g.view.repaint();
    await new Promise(requestAnimationFrame);
    const items = g.core.items;
    const pictures = items.filter((i) => i.kind === 2 && !(i.flags & 16));
    const shown = [...g.view.element.querySelectorAll(".gallery-content img.g-picture:not([hidden])")]
      .filter((img) => img.style.visibility !== "hidden" && img.src).length;
    const lost = { items: items.length, faded: items.filter((i) => i.flags & 128).length,
                   card: items.filter((i) => i.flags & 16).length, pictures: pictures.length, picturesShown: shown,
                   ...measure() };
    if (params.has("shots")) {
      await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-lost`)}`);
      await sleep(1500);
    }
    folders.walk = walk;
    folders.file = file;
    await g.reconnected();
    await settle();
    const back = { faded: g.core.items.filter((i) => i.flags & 128).length, items: g.core.items.length };
    await post({ folder: dir, lost, back });
    return;
  }
  if (params.has("poster")) {
    // The folder as a poster in each view: aligned as painted, and drawn as PNG.
    const posters = [];
    const names = ["plates", "sheet", "ladders", "atlas", "catalogue"];
    for (const view of params.get("views") === "all" ? [0, 1, 2, 3, 4] : [0, 1]) {
      await g.show(root, "");
      g.setView(view);
      setGalleryPoster(true);
      await sleep(600);
      await settle();
      const run = { view, ...measure(), contentHeight: Math.round(g.core.contentHeight) };
      const png = await g.view.posterPNG();
      run.pngBytes = png?.size ?? 0;
      // Its PDF too: a page, or in Catalogue the book, a spread a page.
      const pdfStart = performance.now();
      let counted = 0, label = "";
      const pdf = await g.posterPDF((page, pages) => {
        counted++;
        galleryToolbar.savingPDF(`Page ${page} of ${pages}\u2026`); // as the button counts them
        label = galleryToolbar.posterBar.textContent;
      });
      galleryToolbar.savingPDF(null);
      run.pdfPagesCounted = counted;
      run.pdfLabel = label;
      run.pdfBytes = pdf.size;
      run.pdfMs = Math.round(performance.now() - pdfStart);
      await fetch(`/png?type=pdf&label=${encodeURIComponent(`${params.get("label") ?? "web"}-poster-${names[view]}`)}`,
                  { method: "POST", body: pdf });
      if (png) await fetch(`/png?label=${encodeURIComponent(`${params.get("label") ?? "web"}-poster-${names[view]}`)}`, { method: "POST", body: png });
      if (params.has("shots")) {
        await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-poster-screen-${view}`)}`);
        await sleep(1500);
      }
      setGalleryPoster(false);
      posters.push(run);
    }
    await post({ folder: dir, posters, groupsOver: over, maxDeviation: Math.round(deviation * 1000) / 1000 });
    return;
  }
  const g3 = params.has("scrollOnly") ? null : await galleryMotionChecks(g, root, subfolders, measure, settle);
  // Scrolling the tallest folder: long tasks over 50 ms while it moves.
  const tall = params.get("scroll") ?? subfolders.at(-1) ?? "";
  await g.show(root, tall);
  await settle();
  if (params.has("expand")) {
    // Every size band shown whole: "show all" on each, then their pictures.
    for (let band = 0; band < 12; band++) g.view.act(g.core.click(3, band, false));
    await settle();
  }
  const longTasks = [];
  const observer = new PerformanceObserver((list) => {
    for (const entry of list.getEntries()) longTasks.push(Math.round(entry.duration));
  });
  try {
    observer.observe({ type: "longtask", buffered: false });
  } catch {
    // No long tasks reported here.
  }
  const clip = g.view.element.querySelector(".gallery-clip");
  const height = clip.scrollHeight - clip.clientHeight;
  const scrollStarted = performance.now();
  for (let i = 1; i <= 120; i++) {
    clip.scrollTop = (height * i) / 120;
    await new Promise(requestAnimationFrame);
  }
  const scrollMs = Math.round(performance.now() - scrollStarted);
  await sleep(200);
  observer.disconnect();
  // No 3D frames while the gallery covers the view.
  const before = engine.stats().frames;
  await sleep(2000);
  const framesWhileCovered = engine.stats().frames - before;
  // Detail pictures: Catalogue's plate figure, drawn large.
  const details = { galleryBefore: galleryShown };
  if (views.includes(4)) {
    await g.show(root, "");
    g.setView(4);
    g.catalogue({ spread: 2 });
    await settle();
    const detailStart = performance.now();
    const wanted = g.core.details().length;
    while (g.view.detailCount < wanted && performance.now() - detailStart < 60_000) await sleep(200);
    Object.assign(details, { catalogueWanted: wanted, catalogueShown: g.view.detailCount,
                             catalogueSeconds: Math.round(performance.now() - detailStart) / 1000 });
    if (params.has("shots")) {
      await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-detail-catalogue`)}`);
      await sleep(1500);
    }
  }
  // What the person found trying the gallery: Esc with the Look card open closes the card first,
  // and × leaves the gallery.
  const galleryBefore = galleryShown;
  toggleCard(lookCard);
  const lookOpened = !lookCard.element.hidden;
  // Pressed as a key is, at the page's focus: the window's capturing listener sees it first.
  document.body.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", code: "Escape", bubbles: true }));
  const escape = { galleryBefore, lookOpened, lookOpenAfter: !lookCard.element.hidden, galleryAfter: galleryShown };
  masthead.onClose();
  const close = { galleryAfter: galleryShown, viewHidden: g.view.element.hidden,
                  emptyShown: !status.element.hidden };
  await post({ folder: dir, files: listing.filter((e) => !e.folder).length, runs, groupsOver: over,
               window: { escape, close, details },
               worstSpread: worst, maxDeviation: Math.round(deviation * 1000) / 1000,
               scroll: { folder: tall, height: Math.round(height), frames: 120, ms: scrollMs, longTasks,
                         items: g.core.items.length,
                         longest: Math.max(0, ...longTasks) },
               framesWhileCovered, ...(g3 ? { g3 } : {}),
               // Nothing scrolls sideways, the page or the gallery, at any width.
               overflowX: { page: document.scrollingElement.scrollWidth - innerWidth,
                            gallery: clip.scrollWidth - clip.clientWidth, width: innerWidth } });
}

// G3's checks in the page: a heading of Sheet's stays at the top as its block scrolls under it; a
// search fades what it did not find; Peek is painted over the view and aligned; and each move plays
// for as long as the spec says, the selection kept.
async function galleryMotionChecks(g, root, subfolders, measure, settle) {
  const clip = g.view.element.querySelector(".gallery-clip");
  const out = {};
  // Sticky headings, in Sheet at the top, scrolled a third of the way.
  await g.show(root, "");
  g.setView(1);
  await sleep(600);
  await settle();
  clip.scrollTop = Math.round((clip.scrollHeight - clip.clientHeight) / 3);
  await new Promise(requestAnimationFrame);
  await new Promise(requestAnimationFrame);
  const top = clip.getBoundingClientRect().top;
  const items = g.core.items;
  let stuck = null;
  for (const [i, box] of g.view.paintedBoxes()) {
    const item = items[i];
    if (item.kind !== 0 || !(item.stickyUntil > 0)) continue;
    const dy = clip.scrollTop - item.stickyTop;
    if (dy > 0 && dy < item.stickyUntil) {
      // On screen, the band's top sits at the view's top.
      const node = [...clip.querySelectorAll(".g-rect")].find((n) => !n.hidden && Number(n.style.zIndex) === items.length + i);
      stuck = { stickyTop: item.stickyTop, scroll: clip.scrollTop,
                fromViewTop: node ? Math.round((node.getBoundingClientRect().top - top) * 1000) / 1000 : null,
                layoutTop: Math.round(box.y * 1000) / 1000 };
      break;
    }
  }
  out.sticky = { ...stuck, aligned: measure() };
  clip.scrollTop = 0;
  // A search: what it did not find painted at 22%.
  const probe = params.get("search") ?? g.core.path(g.core.wanted()[0]?.file ?? 0).split("/").pop().slice(0, 3);
  g.setSearch(probe);
  await new Promise(requestAnimationFrame);
  if (params.has("shots")) {
    await settle();
    await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-search`)}`);
    await sleep(1500);
  }
  const faded = [...g.view.element.querySelectorAll(".gallery-content > :not([hidden])")].filter((n) => n.style.opacity === "0.22").length;
  out.search = { query: probe, found: g.core.matches, fadedNodes: faded, aligned: measure() };
  g.setSearch("");
  // Peek over the first file of the first subfolder, in Sheet, where → selects a file. A folder
  // shows the view it last showed, so the view is chosen once in it.
  await g.show(root, subfolders[0] ?? "");
  g.setView(1);
  await sleep(600);
  await settle();
  g.view.key(1); // → selects the first file
  g.view.key(5); // Space
  await new Promise(requestAnimationFrame);
  const fixed = g.view.element.querySelectorAll(".gallery-fixed > :not([hidden])").length;
  out.peek = { file: g.core.peeking === null ? null : g.core.path(g.core.peeking), fixedNodes: fixed, aligned: measure() };
  if (params.has("shots")) {
    await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-peek`)}`);
    await sleep(1500);
  }
  // Peek's turntable: its frames come, then → turns the part a frame at a time.
  const turnStart = performance.now();
  while (!g.core.peekTurnable && performance.now() - turnStart < 60_000) await sleep(100);
  await new Promise(requestAnimationFrame);
  const turn = { seconds: Math.round(performance.now() - turnStart) / 1000, turnable: g.core.peekTurnable,
                 aligned: measure() };
  if (params.has("shots")) {
    await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-peek-turn-0`)}`);
    await sleep(1500);
  }
  for (let i = 0; i < 6; i++) g.view.key(1); // a quarter turn, at 24 frames
  turn.frameAfterSix = g.view.turnFrameShown;
  turn.peekingStill = g.core.peeking === null ? null : g.core.path(g.core.peeking);
  if (params.has("shots")) {
    await fetch(`/shot?name=${encodeURIComponent(`${params.get("label") ?? "web"}-gallery-peek-turn-6`)}`);
    await sleep(1500);
  }
  out.peekTurn = turn;
  g.view.key(6); // Esc closes Peek
  // A switch from Plates to Sheet, which shows more: its transition and tracks, and the animations
  // playing. Parts in both fly; those new to Sheet fade in.
  g.setView(0);
  await sleep(700);
  const selected = g.core.selected;
  g.setView(1);
  const t = g.core.transition, tracks = g.core.tracks;
  const playing = document.getAnimations().map((a) => a.effect?.getTiming?.()).filter(Boolean);
  out.switch = {
    transition: { kind: t.kind, duration: t.duration, fadeOut: t.fadeOut },
    fly: tracks.filter((k) => k.kind === 0).length, fadeIn: tracks.filter((k) => k.kind === 1).length,
    flyTiming: [...new Set(tracks.filter((k) => k.kind === 0).map((k) => `${k.delay}+${k.duration}`))],
    fadeTiming: [...new Set(tracks.filter((k) => k.kind === 1).map((k) => `${Math.round(k.delay)}+${Math.round(k.duration)}`))],
    animations: playing.length,
    animationTimings: [...new Set(playing.map((p) => `${Math.round(p.delay)}+${Math.round(p.duration)}`))],
    selectionKept: g.core.selected === selected && selected !== null,
  };
  await sleep(700);
  // Into a folder from its plate, and back out.
  await g.show(root, "");
  g.setView(0);
  await sleep(600);
  await settle();
  const plate = g.core.items.find((i) => i.targetKind === 1);
  if (plate) {
    g.view.act(g.core.click(1, plate.target, false));
    const into = g.core.transition;
    const zoom = document.getAnimations().map((a) => a.effect?.getTiming?.()).filter(Boolean);
    await sleep(600);
    g.view.key(6); // Esc: up again
    const back = g.core.transition;
    out.zoom = { into: { kind: into.kind, duration: into.duration, rect: into.rect.map(Math.round) },
                 out: { kind: back.kind, duration: back.duration, rect: back.rect.map(Math.round) },
                 animations: zoom.length, animationTimings: [...new Set(zoom.map((p) => `${Math.round(p.delay)}+${Math.round(p.duration)}`))] };
    await sleep(600);
  }
  return out;
}

// Waits for an imported file's scene, zooms in on the middle of the view, and reports how many
// triangles refinement brought, and how long after the view settled they were all in.
async function runRefineTest() {
  const ready = await waitFor(Events.SceneReady);
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  await sleep(1500);
  const before = engine.stats().triangles;
  const { Pointer, Buttons } = await import("./engine.js");
  for (let i = 0; i < Number(params.get("zoom") ?? 6); ++i) {
    engine.pointer(Pointer.Scroll, Buttons.None, canvas.width / 2, canvas.height / 2, 120);
    await new Promise(requestAnimationFrame);
  }
  // Refinement starts once the view has been still a moment, and may run several jobs, finest
  // first; a job's segments can all come at its end, and count once a frame has drawn them. Done
  // after 3 s with no job running and the count steady.
  const zoomed = performance.now();
  let triangles = before, changed = zoomed, quietFrom = zoomed + 3000;
  const patience = Number(params.get("wait") ?? 120000);
  while (performance.now() - zoomed < patience) {
    await sleep(100);
    const now = engine.stats().triangles;
    if (now !== triangles) {
      triangles = now;
      changed = performance.now();
    }
    if (refinement) quietFrom = performance.now();
    if (performance.now() - Math.max(changed, quietFrom) >= 3000) break;
  }
  await post({ before, after: triangles, refinedMs: triangles === before ? null : Math.round(changed - zoomed),
               workers: importWorkers, refinements });
}

// The page's calls into the engine while a test watches, each named with its time in ms since
// the watch began: whatever wakes a view that should be still, short of the engine's own clock.
function watchEngineCalls() {
  const calls = [];
  let since = null;
  const prototype = Object.getPrototypeOf(engine);
  for (const name of Object.getOwnPropertyNames(prototype)) {
    const original = Object.getOwnPropertyDescriptor(prototype, name).value;
    if (name === "constructor" || name === "stats" || typeof original !== "function") continue;
    engine[name] = function (...args) {
      if (since !== null && calls.length < 20) calls.push(`${name}@${Math.round(performance.now() - since)}`);
      return original.apply(this, args);
    };
  }
  return {
    start() { calls.length = 0; since = performance.now(); },
    stop() { since = null; return calls.slice(); },
  };
}

// Waits for the scene, then measures: frames submitted while nothing happens, frames while the
// pointer drags the view, hover picks, and the memory held.
async function runTest() {
  const opened = performance.now();
  const ready = await waitFor(Events.SceneReady);
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  const loadMs = performance.now() - opened;
  const watch = watchEngineCalls();
  await sleep(1500);
  const settled = engine.stats();
  watch.start();
  await sleep(3000);
  const idle = engine.stats();
  const idleCalls = watch.stop();
  // A drag across the middle, one move per frame for 2 s.
  const { width, height } = canvas;
  const { Pointer, Buttons } = await import("./engine.js");
  engine.pointer(Pointer.Down, Buttons.Left, width / 2, height / 2);
  const dragStart = engine.stats();
  const started = performance.now();
  for (let i = 1; i <= 120; ++i) {
    engine.pointer(Pointer.Move, Buttons.None, width / 2 + i * 4, height / 2 + i);
    await new Promise(requestAnimationFrame);
  }
  engine.pointer(Pointer.Up, Buttons.Left, width / 2 + 480, height / 2 + 120);
  const dragMs = performance.now() - started;
  const dragged = engine.stats();
  // Hover over the middle, where the model is.
  engine.pointer(Pointer.Move, Buttons.None, width / 2, height / 2);
  watch.start();
  await sleep(1000);
  const hover = engine.stats();
  await sleep(2000);
  const after = engine.stats();
  const afterCalls = watch.stop();
  if (!params.has("nosnapshot")) await postSnapshot();
  await post({
    width, height, startupMs, loadMs: Math.round(loadMs),
    idleFrames: idle.frames - settled.frames, idleSettledFrames: idle.settledFrames - settled.settledFrames,
    looping: idle.looping, loopCause: idle.loopCause, idleCalls,
    dragFrames: dragged.frames - dragStart.frames, dragMs: Math.round(dragMs),
    gpuMs: dragged.gpuMs, hovered: hover.hovered,
    settledAfterDrag: after.settledFrames - dragged.settledFrames,
    loopingAfter: after.looping, loopCauseAfter: after.loopCause, afterCalls,
    bufferMB: after.bufferMB, targetMB: after.targetMB, heapMB: after.heapMB,
    triangles: after.triangles, read: engine.lastRead,
    // The toolbar's tools that show, by name: what folds into More on this screen.
    tools: [...toolbar.element.querySelectorAll("[data-tool]")].filter((e) => e.getClientRects().length > 0)
      .map((e) => e.dataset.tool),
  });
}
