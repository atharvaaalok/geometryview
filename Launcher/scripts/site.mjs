// Assembles the hosted viewer, as GitHub Pages serves it: the viewer's files (scripts/web.mjs), and
// models to try in samples/, listed in samples/index.json, which the page offers on its first
// page. Zips it all into dist/geometryview-web.zip, which a release carries and the Pages workflow
// deploys. The models are GeometryView's own showcase (Tools/showcase/make.py writes them into
// build/showcase), or come from GEOMETRYVIEW_SHOWCASE.
// Each is published under a plain name: the string its header names it by (FILE_NAME), which is
// also its root assembly's name when it has one, becomes the sample's root everywhere it appears.
//
// node scripts/site.mjs [--no-zip]   leaves the site in dist/site either way

import { execFileSync } from "node:child_process";
import { cpSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const showcase = process.env.GEOMETRYVIEW_SHOWCASE ?? join(launcher, "../build/showcase");
const folder = "library/Things That Come in Sizes";
const site = join(launcher, "dist/site");
const zip = join(launcher, "dist/geometryview-web.zip");

// In the order the page lists them.
const kSamples = [
  { id: "turbofan", root: "tf1_turbofan", name: "TF-1 turbofan", from: "step/turbofan.step", file: "turbofan.step",
    about: "A turbofan on its stand, 1,586 bodies: cut it along its axis to see every stage." },
  { id: "lens", root: "lens_50mm_f1_4", name: "LN-50 camera lens", from: "step/lens.step", file: "camera-lens.step",
    about: "A 50 mm f/1.4 lens: turn its housing to glass to see the seven elements and the iris." },
  { id: "orrery", root: "orrery", name: "OR-1 orrery", from: "step/orrery.step", file: "orrery.step",
    about: "A desk orrery, its planets turned by a motor and gear train hidden in the base." },
  { id: "dolls", root: "matryoshka_7", name: "Nesting dolls", from: `${folder}/06_nesting_dolls/matryoshka/007 Matryoshka 7.step`,
    file: "nesting-dolls.step", about: "Seven matryoshka, one inside the other: a cut shows rings within rings." },
  { id: "knight", root: "knight_95mm", name: "Chess knight", from: `${folder}/02_chessmen/tournament_set/003 Knight.step`,
    file: "chess-knight.step", about: "A single part: a tournament knight, 95 mm tall." },
  { id: "parade", root: "parade", name: "The Parade", from: "step/parade.step", file: "parade.step",
    about: "100,000 chessmen in rows, to feel the speed." },
];

execFileSync(process.execPath, [join(launcher, "scripts/web.mjs")], { stdio: "inherit" });
rmSync(site, { recursive: true, force: true });
cpSync(join(launcher, "web"), site, { recursive: true });
mkdirSync(join(site, "samples"));
const samples = kSamples.map(({ from, root, ...sample }) => {
  const to = join(site, "samples", sample.file);
  writeFileSync(to, renamed(readFileSync(join(showcase, from), "latin1"), root), "latin1");
  return { ...sample, size: statSync(to).size };
});
writeFileSync(join(site, "samples/index.json"), JSON.stringify({ samples }, null, 2) + "\n");

// The STEP text with the name its header gives it, as a whole quoted string, replaced by `root`.
function renamed(step, root) {
  const name = /FILE_NAME\s*\(\s*'((?:[^']|'')*)'/.exec(step)?.[1];
  return name ? step.split(`'${name}'`).join(`'${root}'`) : step;
}

if (!process.argv.includes("--no-zip")) {
  rmSync(zip, { force: true });
  execFileSync("zip", ["-qr", "-X", zip, "."], { cwd: site });
  console.log(zip);
}
console.log(site);
