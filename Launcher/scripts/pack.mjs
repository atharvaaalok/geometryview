// Packs the tool server, the viewer and, on a Mac, the native engine into
// dist/geometry-viewer.mcpb, the one-click extension format Claude Desktop installs. Run after the
// release preset and the WebAssembly presets are built (see scripts/web.mjs).

import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const build = join(launcher, "../build/release");
const stage = join(launcher, "dist/stage");
const bundle = join(launcher, "dist/geometry-viewer.mcpb");

execFileSync(process.execPath, [join(launcher, "scripts/web.mjs")], { stdio: "inherit" });
rmSync(join(launcher, "dist"), { recursive: true, force: true });
mkdirSync(stage, { recursive: true });
cpSync(join(launcher, "mcpb/manifest.json"), join(stage, "manifest.json"));
cpSync(join(launcher, "package.json"), join(stage, "package.json"));
for (const folder of ["bin", "src", "web"]) {
  cpSync(join(launcher, folder), join(stage, folder), { recursive: true });
}
// The native engine, where it was built: a Mac's pictures then need no browser.
if (existsSync(join(build, "Tools/gcli"))) {
  mkdirSync(join(stage, "native"), { recursive: true });
  cpSync(join(build, "Tools/gcli"), join(stage, "native/gcli"));
  cpSync(join(build, "Import/geometry-occt-worker"), join(stage, "native/geometry-occt-worker"));
  cpSync(join(build, "shaders/default.metallib"), join(stage, "native/default.metallib"));
}
execFileSync("zip", ["-qr", bundle, "."], { cwd: stage });
rmSync(stage, { recursive: true });
console.log(bundle);
