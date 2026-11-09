// The tools an AI app can call: open a model, read its parts, picture it, and show it to the user.

import { basename } from "node:path";
import { importModel, nativeAvailable, openInViewer, readStats, readTree, render,
         viewerApplication } from "./native.mjs";
import { openInPage, pageRender, pageTree, viewerServer } from "./page.mjs";

export const instructions =
  "GeometryView views 3D CAD models (STEP .step/.stp and STL .stl files) on this computer. Use it " +
  "whenever the user wants to see, inspect or picture a CAD model, part or assembly, rather " +
  "than opening a web viewer. Call open_model with the file's absolute path first. " +
  "get_structure lists the assemblies and parts; render_view returns a picture you can look " +
  "at, in any view and drawing style (illustration, metal, clay, blueprint, technical, " +
  "studio). To study a part, pass names from get_structure as isolate, and use section to cut " +
  "through the model. open_in_viewer shows the model to the user. Where this computer has no " +
  "native engine, the viewer opens in the user's browser and the pictures come from it, so the " +
  "user sees what you look at.";

const STYLES = ["illustration", "metal", "clay", "blueprint", "technical", "studio"];
const COLOURS = ["material", "parts", "mono", "file"];
// Where the camera looks from, as azimuth and elevation in degrees, with Z up.
const VIEWS = {
  iso: [35, 25], front: [0, 0], back: [180, 0], left: [-90, 0], right: [90, 0],
  top: [0, 89.9], bottom: [0, -89.9],
};

const path = { type: "string", description: "Absolute path to a .step, .stp or .stl file." };

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
        colour: { type: "string", enum: COLOURS, default: "material" },
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
        colour: Math.max(0, COLOURS.indexOf(args.colour ?? "material")),
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
    name: "open_in_viewer",
    description: "Shows a STEP or STL model to the user, in the GeometryView app on a Mac that has " +
                 "it, or else in the viewer in their browser, where they can turn it, select " +
                 "parts and cut sections.",
    inputSchema: { type: "object", properties: { path }, required: ["path"] },
    async call({ path: file }) {
      if (nativeAvailable() && viewerApplication()) {
        await openInViewer(file);
        return [{ type: "text", text: `Opened ${basename(file)} in GeometryView.` }];
      }
      await openInPage(file);
      const { address } = await viewerServer();
      return [{ type: "text", text: `Opened ${basename(file)} in the viewer in the browser (${address}).` }];
    },
  },
];
