// The universal viewer's page: the engine on a full-window canvas, and the chrome around it, laid
// out as the Mac app lays out its window.
//
// viewer/?model=<fixture>        opens <fixtures>/<fixture>.geoblob (fixtures= moves the folder)
// viewer/?test=idle&label=...    runs a scripted check and posts its report (see runTest)

import { Actions, Engine, Events, NodeKinds, Tools } from "./engine.js";
import { ViewportInput } from "./input.js";
import { StructureCard } from "./structure.js";
import { LibraryCard, cachedImport, keptFile } from "./library.js";
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

const status = new StatusCard(() => chooseFile(false));
document.body.append(status.element);
// The engine may speak while it starts, before the chrome below exists to listen.
let chromeReady = false;

let engine;
try {
  engine = await Engine.start(canvas, { event: handle }, { phone });
} catch (error) {
  status.showFailure(`This browser cannot run the viewer: ${error.message}`);
  throw error;
}
window.geometry = engine; // for the console and the shell's checks
// From navigation to an engine ready to draw: the page, the module, the GPU and the overlay's text.
const startupMs = Math.round(performance.now());

let look = Look.saved();
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
const library = new LibraryCard({
  onOpen: (file, name, add) => openCache(file, name, add),
  onChoose: () => chooseFile(false),
  onSwitch: (show) => showLibrary(show),
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
addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "o") {
    e.preventDefault();
    chooseFile(e.shiftKey);
  }
});
// A press in the view closes the look and section cards and the views menu, as clicking away
// does on the Mac.
canvas.addEventListener("pointerdown", () => {
  closeCards();
  toolbar.viewsMenu.hidden = true;
});
chromeReady = true;
applyLook(look);
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
      progress.set(document, total > 0 ? done / total : 0);
      refreshMasthead();
      break;
    case Events.ModelReady:
      structure.modelReady(document);
      status.hide();
      refreshMasthead();
      toolbar.update({ hasSelection: false, hasModel: true });
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
    const counts = `${parts.toLocaleString("en-US")} parts · ${bodies.toLocaleString("en-US")} bodies`;
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

function applyLook(next) {
  look = next;
  look.save();
  engine.setLook(look.engineLook);
  const dark = Styles[look.style].title === "Blueprint";
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

function showEmpty() {
  status.showInvitation();
  structure.element.hidden = true;
  library.element.hidden = true;
  library.refresh().then(() => {
    if (names.length === 0 && panelOpen && library.element.querySelector(".entry")) showLibrary(true);
  });
  updateInsets();
  toolbar.update({ hasSelection: false, hasModel: false });
}

function showLibrary(show) {
  if (show) library.refresh();
  library.element.hidden = !show || !panelOpen;
  structure.element.hidden = show || names.length === 0 || !panelOpen;
  library.switcher.selected = show ? 0 : 1;
  structure.switcher.selected = show ? 0 : 1;
  updateInsets();
}

// ---- Files ------------------------------------------------------------------------------------

function chooseFile(adding) {
  picker.dataset.adding = adding ? "1" : "0";
  picker.click();
}

function forget() {
  names = [];
  progress.clear();
  structure.reset();
  selection.element.hidden = true;
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
const importWorkers = phone ? 2 : Math.max(2, Math.min(12, (navigator.hardwareConcurrency ?? 4) - 2));

function importFile(file, add) {
  importing = importing.then(async () => {
    const cached = await cachedImport(file);
    if (cached) {
      openCache(cached, file.name, add);
      return { type: "cached" };
    }
    // STEP files go to the long-lived importer, whose OCCT pool stays warm; an STL file to one
    // of its own, ended afterwards, which frees its threads and memory.
    const stl = /\.stl$/i.test(file.name);
    const worker = stl ? new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" })
                       : (importer ??= new Worker(new URL("./import-worker.js", import.meta.url), { type: "module" }));
    status.showImporting(file.name, 0, 0);
    const result = await new Promise((resolve) => {
      worker.onmessage = ({ data }) => {
        if (data.type === "progress") status.showImporting(file.name, data.done, data.total);
        else resolve(data);
      };
      worker.onerror = (event) => resolve({ type: "failed", message: event.message ?? "The importer failed." });
      worker.postMessage({ type: "import", file, kind: stl ? "stl" : "step", name: file.name,
                           workers: importWorkers, threads: stl });
    });
    if (stl) worker.terminate();
    lastImport = result;
    if (result.type !== "done") {
      status.showFailure(result.message);
      return result;
    }
    openCache(await keptFile(file.name), file.name, add);
    return result;
  });
  return importing;
}

let lastImport = null;
let importStarted = 0;

// Removes a kept import, so the next open imports again.
async function forgetImport(name) {
  try {
    const folder = await (await navigator.storage.getDirectory()).getDirectoryHandle("library");
    await folder.removeEntry(`${name}.geoblob`);
    await folder.removeEntry(`${name}.json`);
  } catch {
    // Nothing kept.
  }
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
addEventListener("drop", (e) => {
  e.preventDefault();
  dragDepth = 0;
  dropZone.element.hidden = true;
  openFiles([...e.dataTransfer.files], e.metaKey || e.ctrlKey);
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
window.geometryShell = {
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
    async render({ azimuth = 35, elevation = 25, perspective: persp = false, style, colour,
                   isolate = [], section = [], width = 1200, height = 800 }) {
      if (names.length === 0) throw new Error("Nothing is open in the viewer.");
      const next = Object.assign(new Look(), look);
      if (style !== undefined) next.choose(style);
      if (colour !== undefined) next.colour = colour;
      look = next;
      lookCard.show(next);
      engine.setLook(next.engineLook);

      sectionCard.cuts.forEach((cut) => { cut.enabled = false; });
      for (const cut of section) {
        const axis = "xyz".indexOf(cut.axis);
        if (axis >= 0) Object.assign(sectionCard.cuts[axis], { enabled: true, position: cut.at, flip: Boolean(cut.flip) });
      }
      sectionCard.show();
      engine.setSection(sectionCard.cuts);

      if (isolate.length > 0 || isolatedByRequest) engine.perform(Actions.ShowAll);
      isolatedByRequest = false;
      if (isolate.length > 0) {
        const wanted = isolate.map((n) => n.toLowerCase());
        engine.perform(Actions.ClearSelection);
        let found = 0;
        structure.tree.documents.forEach((d, document) => {
          d.model?.forEach((n, node) => {
            if (n.kind !== NodeKinds.Body && wanted.some((w) => n.name.toLowerCase().includes(w))) {
              engine.selectNode(document, node, true);
              found += 1;
            }
          });
        });
        if (found === 0) throw new Error(`No part's name contains ${isolate.join(" or ")}.`);
        engine.perform(Actions.Isolate);
        engine.perform(Actions.ClearSelection);
        isolatedByRequest = true;
      }

      perspective = persp;
      toolbar.projection.selected = persp ? 1 : 0;
      engine.setView(azimuth, elevation, persp);
      const image = await engine.snapshot();
      if (!image) throw new Error("The viewer could not draw a picture.");

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
  return `${params.get("label") ?? "unlabelled"}-p4-${params.get("test")}-${model}`;
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
  const ready = await waitFor(Events.SceneReady);
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  const totalMs = performance.now() - importStarted;
  await sleep(1500);
  const stats = engine.stats();
  if (!params.has("nosnapshot")) await postSnapshot();
  await post({ totalMs: Math.round(totalMs), import: lastImport, cached: lastImport === null,
               workers: importWorkers, triangles: stats.triangles, bufferMB: stats.bufferMB,
               heapMB: stats.heapMB });
}

// Waits for the scene, then measures: frames submitted while nothing happens, frames while the
// pointer drags the view, hover picks, and the memory held.
async function runTest() {
  const opened = performance.now();
  const ready = await waitFor(Events.SceneReady);
  if (ready.kind === Events.Failed) throw new Error(ready.message);
  const loadMs = performance.now() - opened;
  await sleep(1500);
  const settled = engine.stats();
  await sleep(3000);
  const idle = engine.stats();
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
  await sleep(1000);
  const hover = engine.stats();
  await sleep(2000);
  const after = engine.stats();
  if (!params.has("nosnapshot")) await postSnapshot();
  await post({
    width, height, startupMs, loadMs: Math.round(loadMs),
    idleFrames: idle.frames - settled.frames, idleSettledFrames: idle.settledFrames - settled.settledFrames,
    looping: idle.looping,
    dragFrames: dragged.frames - dragStart.frames, dragMs: Math.round(dragMs),
    gpuMs: dragged.gpuMs, hovered: hover.hovered,
    settledAfterDrag: after.settledFrames - dragged.settledFrames,
    loopingAfter: after.looping,
    bufferMB: after.bufferMB, targetMB: after.targetMB, heapMB: after.heapMB,
    triangles: after.triangles,
  });
}
