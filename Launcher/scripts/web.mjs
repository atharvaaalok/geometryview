// Gathers the viewer's files into Launcher/web, as the launcher serves them when packed: the page,
// its engine, its importers and its fonts. Run after the wasm-webgpu, wasm-release and
// wasm-threads presets are built.

import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const root = join(launcher, "..");
const web = join(launcher, "web");

const copies = [
  [join(root, "Web/viewer"), web],
  [join(root, "build/wasm-webgpu/Engine/geometry-web.mjs"), join(web, "build/geometry-web.mjs")],
  [join(root, "build/wasm-webgpu/Engine/geometry-web.wasm"), join(web, "build/geometry-web.wasm")],
  [join(root, "build/wasm-release/Import/geometry-step-web.mjs"), join(web, "occt/geometry-step-web.mjs")],
  [join(root, "build/wasm-release/Import/geometry-step-web.wasm"), join(web, "occt/geometry-step-web.wasm")],
  [join(root, "build/wasm-release/Import/geometry-occt-web.mjs"), join(web, "occt/geometry-occt-web.mjs")],
  [join(root, "build/wasm-release/Import/geometry-occt-web.wasm"), join(web, "occt/geometry-occt-web.wasm")],
  [join(root, "build/wasm-threads/Import/geometry-step-web.mjs"), join(web, "occt-threads/geometry-step-web.mjs")],
  [join(root, "build/wasm-threads/Import/geometry-step-web.wasm"), join(web, "occt-threads/geometry-step-web.wasm")],
  ...["Regular", "Medium", "SemiBold"].map((weight) => [
    join(root, `ThirdParty/ibm-plex-mono/IBMPlexMono-${weight}.ttf`),
    join(web, `fonts/IBMPlexMono-${weight}.ttf`)]),
  [join(root, "ThirdParty/ibm-plex-mono/LICENSE.txt"), join(web, "fonts/LICENSE.txt")],
];

const missing = copies.filter(([from]) => !existsSync(from)).map(([from]) => from);
if (missing.length > 0) {
  console.error(`Build these first:\n  ${missing.join("\n  ")}`);
  process.exit(1);
}
rmSync(web, { recursive: true, force: true });
for (const [from, to] of copies) {
  mkdirSync(dirname(to), { recursive: true });
  cpSync(from, to, { recursive: true });
}
console.log(web);
