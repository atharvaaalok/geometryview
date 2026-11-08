// Packs the tool server and the native engine it drives into dist/geometry-viewer.mcpb, the
// one-click extension format Claude Desktop installs. Run after `cmake --build --preset release`.

import { execFileSync } from "node:child_process";
import { cpSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const build = join(launcher, "../build/release");
const stage = join(launcher, "dist/stage");
const bundle = join(launcher, "dist/geometry-viewer.mcpb");

rmSync(join(launcher, "dist"), { recursive: true, force: true });
mkdirSync(join(stage, "native"), { recursive: true });
cpSync(join(launcher, "mcpb/manifest.json"), join(stage, "manifest.json"));
cpSync(join(launcher, "package.json"), join(stage, "package.json"));
cpSync(join(launcher, "bin"), join(stage, "bin"), { recursive: true });
cpSync(join(launcher, "src"), join(stage, "src"), { recursive: true });
cpSync(join(build, "Tools/gcli"), join(stage, "native/gcli"));
cpSync(join(build, "Import/geometry-occt-worker"), join(stage, "native/geometry-occt-worker"));
cpSync(join(build, "shaders/default.metallib"), join(stage, "native/default.metallib"));
execFileSync("zip", ["-qr", bundle, "."], { cwd: stage });
rmSync(stage, { recursive: true });
console.log(bundle);
