// Packs the tool server, the viewer and, on a Mac, the native engine into
// dist/geometryview.mcpb, the one-click extension format Claude Desktop installs. Run after the
// release preset and the WebAssembly presets are built (see scripts/web.mjs).
//
// --portable leaves the native engine out, for a bundle to share: pictures then come from the
// browser viewer on any computer, and nothing unsigned is downloaded to run on a Mac.

import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const build = join(launcher, "../build/release");
const stage = join(launcher, "dist/stage");
const bundle = join(launcher, "dist/geometryview.mcpb");
const portable = process.argv.includes("--portable");

execFileSync(process.execPath, [join(launcher, "scripts/web.mjs")], { stdio: "inherit" });
rmSync(join(launcher, "dist"), { recursive: true, force: true });
mkdirSync(stage, { recursive: true });
cpSync(join(launcher, "mcpb/manifest.json"), join(stage, "manifest.json"));
cpSync(join(launcher, "package.json"), join(stage, "package.json"));
cpSync(join(launcher, "web/icons/icon-512.png"), join(stage, "icon.png"));
cpSync(join(launcher, "web/LICENSE"), join(stage, "LICENSE"));
for (const folder of ["bin", "src", "web"]) {
  cpSync(join(launcher, folder), join(stage, folder), { recursive: true });
}
// The native engine, where it was built, or the signed copies in GEOMETRYVIEW_PACK_NATIVE (a flat
// folder, as Tools/release/mac_release.sh leaves them): a Mac's pictures then need no browser.
const signed = process.env.GEOMETRYVIEW_PACK_NATIVE;
const engine = signed
  ? [join(signed, "gcli"), join(signed, "geometryview-occt-worker"), join(signed, "default.metallib")]
  : [join(build, "Tools/gcli"), join(build, "Import/geometryview-occt-worker"), join(build, "shaders/default.metallib")];
if (!portable && existsSync(engine[0])) {
  mkdirSync(join(stage, "native"), { recursive: true });
  cpSync(engine[0], join(stage, "native/gcli"));
  cpSync(engine[1], join(stage, "native/geometryview-occt-worker"));
  cpSync(engine[2], join(stage, "native/default.metallib"));
}
execFileSync("zip", ["-qr", bundle, "."], { cwd: stage });
rmSync(stage, { recursive: true });
console.log(bundle);
