// The universal viewer's page: the engine on a full-window canvas, and the chrome around it, laid
// out as the Mac app lays out its window.
//
// viewer/?model=<fixture>        opens <fixtures>/<fixture>.geoblob (fixtures= moves the folder)
// viewer/?test=idle&label=...    runs a scripted check and posts its report (see runTest)

import { Actions, Engine, Events, NodeKinds, Tools } from "./engine.js";
import { ViewportInput } from "./input.js";
import { StructureCard } from "./structure.js";
import { LibraryCard, cachedImport, forgetKept, keptFile, keptSource, libraryEntries } from "./library.js";
import { keyOf } from "./library-rows.js";
import { LibraryStore } from "./library-store.js";
import { DropZone, HoldHint, HoverChip, Look, LookCard, Masthead, SectionCard, SelectionCard,
         ShortcutsSheet, StatusCard, Styles, Toolbar } from "./chrome.js";
import { el } from "./ui.js";

const params = new URLSearchParams(location.search);
// Errors, kept for the scripted checks' reports.
const errors = [];
addEventListener("error", (e) => errors.push(`${e.message} at ${e.filename}:${e.lineno}`));
addEventListener("unhandledrejection", (e) => errors.push(String(e.reason?.stack ?? e.reason)));
const canvas = document.getElementById("viewport");
const phone = /iPhone|iPad|Android/.test(navigator.userAgent) ||
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
// From navigation to an engine ready to draw: the page, the module, the GPU and the overlay's text.
const startupMs = Math.round(performance.now());

let look = Look.saved(engine.defaultLook());
let perspective = false;
let names = [];
const progress = new Map();
let hovered = null; // { document, node, face }
// The tree and library card: always open on a desktop, a sheet opened when wanted on a phone.
let panelOpen = !phone;
// What the chrome covers of the view, in CSS pixels, as framing keeps clear of it.
let insets = { left: 0, top: 0, right: 0, bottom: 0 };
let pointer = null; // { x, y } in CSS pixels, while over the view

const masthead = new Masthead();
const structure = new StructureCard(engine, { onSwitch: (show) => showLibrary(show) });
const library = new LibraryCard(libraryStore, {
  open: (row, add) => openFromFolder(row, add),
  reopen: (entry, add) => reopenRecent(entry, add),
  choose: () => chooseFile(false),
  connect: () => connectFolder(),
  switch: (show) => showLibrary(show),
  forget: (key) => forgetRecent(key),
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
  onParts: () => {
    panelOpen = !panelOpen;
    showLibrary(names.length === 0);
  },
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
                     lookCard.element, sectionCard.element, toolbar.element, toolbar.viewsMenu, holdHint.element,
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
});
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
      showLibrary(false);
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
      if (!shortcuts.element.hidden) shortcuts.element.hidden = true;
      else if (!lookCard.element.hidden || !sectionCard.element.hidden) closeCards();
      else engine.perform(Actions.ClearSelection);
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

// Shows `next`, and remembers it unless it is the look the page started with: a look saved only
// once the person changes it keeps following the shared default until then.
function applyLook(next, { remember = true } = {}) {
  look = next;
  if (remember) look.save();
  engine.setLook(look.engineLook);
  const dark = Styles[look.style].onDark;
  masthead.setOnDark(dark);
  document.body.classList.toggle("on-dark", dark);
}

// ---- Layout -----------------------------------------------------------------------------------

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
  await library.refresh();
  for (const id of ids) library.expand(id);
  if (!phone) panelOpen = true;
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
  for (const field of ["handle", "root", "path"]) if (source[field]) entry[field] = source[field];
  try {
    const dropped = await libraryStore.remember(entry);
    for (const old of dropped) if (!names.includes(old.key)) await forgetKept(old.key);
  } catch {
    // Not remembered: the file is open all the same.
  }
  library.setOpen(openKeys());
}

async function forgetRecent(key) {
  await libraryStore.forget(key);
  if (!names.includes(key)) await forgetKept(key);
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
// get two OCCT workers, as P2 measured best; desktops one per core, less two for the page.
let importer = null;
let importing = Promise.resolve();
const stepSources = new Map(); // STEP files imported this visit, by name, for refinement
const importWorkers = phone ? 2 : Math.max(2, Math.min(12, (navigator.hardwareConcurrency ?? 4) - 2));

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
  importing = importing.then(async () => {
    const cached = await cachedImport(file);
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
      worker.postMessage({ type: "import", file, kind: stl ? "stl" : "step", name: file.name,
                           workers: importWorkers, threads: stl, progressive: !stl });
    });
    if (stl) worker.terminate();
    lastImport = result;
    if (result.type !== "done") {
      if (token !== null) engine.finishImport(token, result.message);
      else status.showFailure(result.message);
      return result;
    }
    if (token !== null) engine.finishImport(token);
    else openCache(await keptFile(file.name), file.name, add);
    return result;
  });
  return importing;
}

// ---- Refinement --------------------------------------------------------------------------------

// The engine asks for finer geometry where the settled view is close, as on the Mac: the STEP
// file's definitions it names are imported again at a finer level, and their segments join the
// document. The file is the one imported this visit, or the one the library kept beside its cache.
// Refinements wait their turn behind imports, in the same worker and its warm OCCT pool.
let refinement = null;

function refineDocument(token, name, level, definitions) {
  const job = { cancelled: false };
  refinement = job;
  const asked = performance.now();
  importing = importing.then(async () => {
    let ok = false;
    const started = performance.now();
    let note = "";
    let made = null, segments = 0, refused = 0;
    try {
      const file = job.cancelled ? null : stepSources.get(name) ?? await keptSource(name);
      if (!file) return;
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
      if (!ok) console.warn(`Refining ${name} to level ${level} failed: ${result.message}`);
    } finally {
      refinements.push({ name, level, definitions: definitions.length, waitedMs: Math.round(started - asked),
                         ms: Math.round(performance.now() - started), ok, cancelled: job.cancelled, note,
                         made, segments, refused });
      if (!job.cancelled) engine.finishRefinement(token, ok);
      if (refinement === job) refinement = null;
    }
  });
}

// Each refinement job, as the scripted check reports them.
const refinements = [];

function cancelRefinement() {
  if (refinement) refinement.cancelled = true;
}

let lastImport = null;
let importStarted = 0;

// Removes a kept import, so the next open imports again.
const forgetImport = (name) => forgetKept(name);

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
  const name = (model ?? params.get("file") ?? "none").replaceAll("/", "-");
  return `${params.get("label") ?? "unlabelled"}-p4-${params.get("test")}-${name}`;
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
  if (!params.has("keep")) await forgetKept(name);
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

  lastImport = null;
  const opened = performance.now();
  let ready = waitFor(Events.SceneReady);
  await openFromFolder({ root: id, path: `sub/${name}`, name }, false);
  await ready;
  const firstMs = Math.round(performance.now() - opened);
  const imported = lastImport?.type === "done";
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
  await post({ backend: libraryStore.kind, origin: location.origin, keptBefore, listed, firstMs, imported,
               recentFirst: recent[0]?.key,
               recentWhere: recent[0]?.where, reopenMs, reopenedFromCache,
               idleFrames: idle.frames - settled.frames, looping: idle.looping, rows });
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
  });
}
