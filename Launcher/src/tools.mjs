// The tools an AI app can call: open a model, read its parts, picture it, and show it to the user.

import { basename, relative, resolve } from "node:path";
import { contactSheet, importModel, listModels, nativeAvailable, openInViewer, readFacts, readStats,
         readTree, render, viewerApplication } from "./native.mjs";
import { linkToPage, openInPage, pageRender, pageTree, showInPage, viewerServer } from "./page.mjs";

export const instructions =
  "GeometryView views 3D CAD models (STEP .step/.stp and STL .stl files) on this computer. Use it " +
  "whenever the user wants to see, inspect or picture a CAD model, part or assembly, rather " +
  "than opening a web viewer. Call open_model with the file's absolute path first. " +
  "get_structure lists the assemblies and parts; render_view returns a picture you can look " +
  "at, in any view and drawing style (illustration, metal, clay, blueprint, technical, " +
  "studio). To study a part, pass names from get_structure as isolate, and use section to cut " +
  "through the model. open_in_viewer shows the model to the user, live: in the GeometryView app, " +
  "in their browser, or, when they want to see it inside this chat app, as a link to open in " +
  "the app's own browser (where: link). view_folder pictures a whole folder of models on one " +
  "contact sheet with a numbered index, to survey a library or find a part by eye. Where this " +
  "computer has no native engine, the viewer opens in the user's browser and the pictures come " +
  "from it, so the user sees what you look at.";

const STYLES = ["illustration", "metal", "clay", "blueprint", "technical", "studio"];
const COLOURS = ["material", "parts", "mono", "file"];
// Where the camera looks from, as azimuth and elevation in degrees, with Z up.
const VIEWS = {
  iso: [35, 25], front: [0, 0], back: [180, 0], left: [-90, 0], right: [90, 0],
  top: [0, 89.9], bottom: [0, -89.9],
};

const path = { type: "string", description: "Absolute path to a .step, .stp or .stl file." };

// How long view_folder waits for imports before drawing what it has; the rest keep importing, so
// asking again shows them.
const kFolderDeadline = 90_000;

const naturalOrder = new Intl.Collator("en", { numeric: true, sensitivity: "base" }).compare;

/// A box's sides in millimetres, as the gallery writes them: 40 × 30 × 26.
function sides(facts) {
  if (!Array.isArray(facts?.min) || !Array.isArray(facts?.max)) return "";
  return facts.max.map((high, axis) => {
    const value = high - facts.min[axis];
    return value >= 100 ? String(Math.round(value)) : String(Number(value.toPrecision(3)));
  }).join(" × ");
}

function largestSide(facts) {
  if (!Array.isArray(facts?.min) || !Array.isArray(facts?.max)) return Infinity;
  return Math.max(...facts.max.map((high, axis) => high - facts.min[axis]));
}

/// Waits for `work`, or rejects with `kLate` once `deadline` passes; the work carries on.
const kLate = Symbol("late");
function byDeadline(work, deadline) {
  work.catch(() => {});
  return Promise.race([work, new Promise((_, reject) => {
    setTimeout(() => reject(kLate), Math.max(0, deadline - Date.now())).unref?.();
  })]);
}

// ---- The tree as an outline ---------------------------------------------------------------------

function annotate(nodes) {
  const children = nodes.map(() => []);
  const parts = nodes.map((node) => (node.kind === "part" ? 1 : 0));
  nodes.forEach((node, index) => node.parent >= 0 && children[node.parent].push(index));
  for (let index = nodes.length - 1; index >= 0; --index) {
    if (nodes[index].parent >= 0) {
      parts[nodes[index].parent] += parts[index];
    }
  }
  return { children, parts };
}

function describe(node, index, parts) {
  const name = node.name || `(unnamed ${node.kind})`;
  const bodies = node.bodies[1] - node.bodies[0];
  if (node.kind === "assembly") {
    return `${name} — assembly, ${parts[index]} part${parts[index] === 1 ? "" : "s"}`;
  }
  return bodies > 1 ? `${name} — part, ${bodies} bodies` : `${name} — part`;
}

/// An indented outline of the tree. Bodies are counted on their parts rather than listed. With a
/// filter, only nodes whose names contain it, and their ancestors, are shown.
function outline(nodes, { maxDepth = Infinity, maxLines = 400, filter = "" } = {}) {
  const { children, parts } = annotate(nodes);
  const wanted = new Uint8Array(nodes.length);
  const needle = filter.toLowerCase();
  for (let index = 0; index < nodes.length; ++index) {
    if (!needle || nodes[index].name.toLowerCase().includes(needle)) {
      for (let at = index; at >= 0 && !wanted[at]; at = nodes[at].parent) {
        wanted[at] = 1;
      }
    }
  }
  const lines = [];
  let omitted = 0;
  const walk = (index, depth) => {
    if (!wanted[index] || nodes[index].kind === "body") {
      return;
    }
    if (lines.length >= maxLines || depth > maxDepth) {
      omitted += 1;
      return;
    }
    lines.push(`${"  ".repeat(depth)}${describe(nodes[index], index, parts)}`);
    children[index].forEach((child) => walk(child, depth + 1));
  };
  nodes.forEach((node, index) => node.parent < 0 && walk(index, 0));
  if (omitted > 0) {
    lines.push(`… ${omitted} more not shown; narrow with filter or raise max_lines.`);
  }
  return lines.join("\n");
}

function counts(nodes) {
  const kinds = { assembly: 0, part: 0, body: 0 };
  nodes.forEach((node) => (kinds[node.kind] += 1));
  const bodies = Math.max(0, ...nodes.filter((n) => n.parent < 0).map((n) => n.bodies[1]));
  return { assemblies: kinds.assembly, parts: kinds.part, bodies };
}

const round = (value) => +value.toPrecision(4);

// ---- Tools -------------------------------------------------------------------------------------

export const tools = [
  {
    name: "open_model",
    description: "Opens a 3D CAD model, a STEP (.step, .stp) or STL (.stl) file on this " +
                 "computer, and summarises it: size, counts and top-level assemblies and parts. " +
                 "Call this before the other GeometryView tools.",
    inputSchema: { type: "object", properties: { path }, required: ["path"] },
    async call({ path: file }) {
      const started = performance.now();
      let nodes, stats, how;
      if (nativeAvailable()) {
        const { blob, fromCache } = await importModel(file);
        [nodes, stats] = await Promise.all([readTree(blob), readStats(blob)]);
        how = fromCache ? "from cache" : "imported";
      } else {
        stats = await openInPage(file);
        nodes = await pageTree(file);
        how = "in the browser's viewer";
      }
      const { assemblies, parts, bodies } = counts(nodes);
      const size = stats.max.map((high, axis) => round(high - stats.min[axis]));
      const seconds = ((performance.now() - started) / 1000).toFixed(2);
      return [{
        type: "text",
        text: [
          `Opened ${basename(file)} (${how}, ${seconds} s).`,
          `${assemblies} assemblies, ${parts} parts, ${bodies} bodies; ` +
            `${stats.faces} faces, ${stats.triangles} triangles.`,
          `Bounding box ${size.join(" × ")} (model units), from [${stats.min.map(round)}] to ` +
            `[${stats.max.map(round)}].`,
          "",
          outline(nodes, { maxDepth: 2, maxLines: 60 }),
        ].join("\n"),
      }];
    },
  },
  {
    name: "get_structure",
    description: "Lists a CAD model's assemblies and parts as an indented outline, with " +
                 "counts, to find the names of parts to show alone.",
    inputSchema: {
      type: "object",
      properties: {
        path,
        filter: { type: "string", description: "Only parts whose names contain this." },
        max_lines: { type: "integer", minimum: 1, maximum: 5000, default: 400 },
      },
      required: ["path"],
    },
    async call({ path: file, filter = "", max_lines: maxLines = 400 }) {
      const nodes = nativeAvailable() ? await readTree((await importModel(file)).blob)
                                      : await pageTree(file);
      const text = outline(nodes, { filter, maxLines });
      return [{ type: "text", text: text || `No part's name contains "${filter}".` }];
    },
  },
  {
    name: "render_view",
    description: "Renders a picture of a 3D CAD model (STEP or STL) for you to look at: a " +
                 "standard view or any angle, a drawing style (illustration, metal, clay, " +
                 "blueprint, technical, or studio: polished metal on a dark backdrop), named parts " +
                 "shown alone, and section cuts through it.",
    inputSchema: {
      type: "object",
      properties: {
        path,
        view: { type: "string", enum: Object.keys(VIEWS), default: "iso" },
        azimuth: { type: "number", description: "Degrees around Z, overriding view." },
        elevation: { type: "number", description: "Degrees above the XY plane, overriding view." },
        style: { type: "string", enum: STYLES, default: "illustration" },
        colour: { type: "string", enum: COLOURS, default: "parts" },
        isolate: { type: "array", items: { type: "string" },
                   description: "Show only parts whose names contain one of these." },
        section: {
          type: "array",
          description: "Planes cutting through the model.",
          items: {
            type: "object",
            properties: {
              axis: { type: "string", enum: ["x", "y", "z"] },
              at: { type: "number", minimum: 0, maximum: 1,
                    description: "Where along the axis, from 0 to 1 across the model." },
              flip: { type: "boolean", description: "Keep the other side.", default: false },
            },
            required: ["axis", "at"],
          },
        },
        width: { type: "integer", minimum: 64, maximum: 4096, default: 1200 },
        height: { type: "integer", minimum: 64, maximum: 4096, default: 800 },
        perspective: { type: "boolean", default: false },
      },
      required: ["path"],
    },
    async call(args) {
      const source = args.path;
      const [azimuth, elevation] = VIEWS[args.view ?? "iso"] ?? VIEWS.iso;
      const view = {
        width: args.width ?? 1200,
        height: args.height ?? 800,
        azimuth: args.azimuth ?? azimuth,
        elevation: args.elevation ?? elevation,
        perspective: args.perspective ?? false,
        style: Math.max(0, STYLES.indexOf(args.style ?? "illustration")),
        colour: Math.max(0, COLOURS.indexOf(args.colour ?? "parts")),
        section: (args.section ?? [])
          .map(({ axis, at, flip }) => `${axis}${flip ? "-" : ""}${at}`)
          .join(","),
        isolate: args.isolate ?? [],
        overlay: true,
      };
      const png = nativeAvailable()
        ? await render((await importModel(args.path)).blob, view)
        : await pageRender(args.path, { ...view, section: args.section ?? [] });
      const shown = view.isolate.length ? ` showing only ${view.isolate.join(", ")}` : "";
      return [
        { type: "image", data: png.toString("base64"), mimeType: "image/png" },
        { type: "text", text: `${basename(source)}${shown}, ${args.style ?? "illustration"} ` +
                              `style, from azimuth ${view.azimuth}°, elevation ${view.elevation}°.` },
      ];
    },
  },
  {
    name: "view_folder",
    description: "Pictures a folder of 3D CAD models (STEP and STL files) on one contact sheet, " +
                 "each part drawn over its name and number, for you to look at, with a numbered " +
                 "index of the files: their paths, sizes in millimetres, parts and triangles. Use it " +
                 "to survey a library, find a part by eye, or compare sizes (same_scale). Files are " +
                 "taken in name order, max_files of them; those still importing after 90 seconds " +
                 "are listed, and asking again shows them.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute path to a folder." },
        recursive: { type: "boolean", default: false, description: "Include the folders inside." },
        max_files: { type: "integer", minimum: 1, maximum: 144, default: 36 },
        filter: { type: "string", description: "Only files whose path inside the folder contains this." },
        sort: { type: "string", enum: ["name", "size"], default: "name",
                description: "Order on the sheet: by path, or smallest to largest measured size." },
        style: { type: "string", enum: STYLES, default: "illustration" },
        colour: { type: "string", enum: COLOURS, default: "parts" },
        same_scale: { type: "boolean", default: false,
                      description: "Draw every part at one scale, so sizes compare." },
        cell: { type: "integer", minimum: 96, maximum: 384, default: 192,
                description: "Each picture's size in points." },
        columns: { type: "integer", minimum: 1, maximum: 12 },
      },
      required: ["path"],
    },
    async call(args) {
      if (!nativeAvailable()) {
        throw new Error("view_folder needs GeometryView's native engine, on a Mac with Apple silicon " +
                        "and macOS 26 or later. Use render_view to picture files one by one.");
      }
      const folder = resolve(args.path);
      const max = Math.min(144, Math.max(1, args.max_files ?? 36));
      const filter = (args.filter ?? "").toLowerCase();
      const found = (await listModels(folder, { recursive: args.recursive ?? false }))
        .map((file) => ({ file, path: relative(folder, file) }))
        .filter(({ path: inside }) => !filter || inside.toLowerCase().includes(filter))
        .sort((a, b) => naturalOrder(a.path, b.path));
      const where = `${folder}${args.recursive ? " and its folders" : ""}`;
      if (found.length === 0) {
        return [{ type: "text", text: `No STEP or STL files${filter ? ` matching "${args.filter}"` : ""} in ${where}.` }];
      }
      const chosen = found.slice(0, max);
      // Imported a few at a time, by the deadline.
      const deadline = Date.now() + kFolderDeadline;
      const done = await Promise.all(chosen.map(async (entry) => {
        try {
          const read = (async () => {
            const { blob } = await importModel(entry.file);
            return { blob, facts: await readFacts(blob) };
          })();
          return { ...entry, ...(await byDeadline(read, deadline)) };
        } catch (error) {
          return error === kLate ? { ...entry, late: true } : { ...entry, error: String(error.message ?? error) };
        }
      }));
      const drawn = done.filter((entry) => entry.blob);
      if (args.sort === "size") drawn.sort((a, b) => largestSide(a.facts) - largestSide(b.facts));
      const lines = [];
      let png = null;
      if (drawn.length > 0) {
        const tiles = drawn.map((entry, index) => ({
          blob: entry.blob,
          label: basename(entry.path).replace(/\.(step|stp|stl)$/i, ""),
          sub: [`#${index + 1}`, sides(entry.facts)].filter(Boolean).join(" · "),
        }));
        const sheet = await contactSheet(tiles, {
          cell: args.cell ?? 192, columns: args.columns, style: args.style ?? "illustration",
          colour: args.colour ?? "parts", sameScale: args.same_scale ?? false, title: basename(folder),
        });
        png = sheet.png;
        drawn.forEach((entry, index) => {
          const f = entry.facts ?? {};
          const failed = sheet.tiles[index]?.error;
          const parts = f.parts ? `${f.parts} part${f.parts === 1 ? "" : "s"}` : null;
          const size = sides(f) ? `${sides(f)} mm` : null;
          const triangles = f.triangles ? `${f.triangles.toLocaleString("en-US")} triangles` : null;
          const name = f.name && f.name !== basename(entry.path).replace(/\.[^.]+$/, "") ? `"${f.name}"` : null;
          lines.push(`${index + 1}. ${entry.path}: ${[size, parts, triangles, name, failed && `not drawn: ${failed}`]
            .filter(Boolean).join(", ")}`);
        });
      }
      const late = done.filter((entry) => entry.late).map((entry) => entry.path);
      const failed = done.filter((entry) => entry.error);
      const header = `${drawn.length} of ${found.length} STEP and STL files in ${where}` +
        (filter ? ` matching "${args.filter}"` : "") +
        `, ${args.style ?? "illustration"} style, ${args.sort === "size" ? "smallest to largest" : "by name"}` +
        (args.same_scale ? ", at one scale" : "") + ". Numbered in reading order, as on the sheet.";
      const text = [header, "", ...lines];
      if (found.length > chosen.length) text.push("", `${found.length - chosen.length} more not shown: raise max_files (up to 144) or use filter.`);
      if (late.length) text.push("", `Still importing, not drawn yet (ask again to see them): ${late.join(", ")}.`);
      if (failed.length) text.push("", "Could not be read:", ...failed.map((entry) => `- ${entry.path}: ${entry.error}`));
      const content = [];
      if (png) content.push({ type: "image", data: png.toString("base64"), mimeType: "image/png" });
      content.push({ type: "text", text: text.join("\n") });
      return content;
    },
  },
  {
    name: "open_in_viewer",
    description: "Shows a STEP or STL model to the user, live, where they can turn it, select " +
                 "parts and cut sections: in the GeometryView app on a Mac that has it (where: " +
                 "app), in their web browser (where: browser), or as a link for this chat app to " +
                 "open in its own browser (where: link; open the returned address there, and the " +
                 "model appears once the page loads). It can show only some parts, in a chosen " +
                 "style and colours.",
    inputSchema: {
      type: "object",
      properties: {
        path,
        where: { type: "string", enum: ["app", "browser", "link"],
                 description: "Where to show it. By default the app where it is installed, else " +
                              "the browser." },
        isolate: { type: "array", items: { type: "string" },
                   description: "Show only parts whose names contain one of these." },
        style: { type: "string", enum: STYLES },
        colour: { type: "string", enum: COLOURS },
      },
      required: ["path"],
    },
    async call({ path: file, where, isolate = [], style, colour }) {
      const application = process.platform === "darwin" ? viewerApplication() : null;
      where ??= application ? "app" : "browser";
      const name = basename(file);
      const shown = (isolate.length ? `, showing only ${isolate.join(", ")}` : "") +
                    (style ? `, in ${style} style` : "") + (colour ? `, coloured by ${colour}` : "");
      if (where === "app") {
        if (!application) throw new Error("The GeometryView app is not installed here: use where browser or link.");
        await openInViewer(file, { isolate, style, colour });
        return [{ type: "text", text: `Opened ${name} in GeometryView${shown}.` }];
      }
      const show = { isolate };
      if (style) show.style = STYLES.indexOf(style);
      if (colour) show.colour = COLOURS.indexOf(colour);
      if (where === "link") {
        const { url, shown: now } = await linkToPage(file, show);
        return [{ type: "text", text: now
          ? `Showing ${name}${shown} in the viewer page already open at ${url}.`
          : `Open ${url} in this app's browser to see ${name}${shown}; it appears once the page loads.` }];
      }
      await showInPage(file, show);
      const { address } = await viewerServer();
      return [{ type: "text", text: `Opened ${name}${shown} in the viewer in the browser (${address}).` }];
    },
  },
];
