// Assembles the hosted viewer, as GitHub Pages serves it: the viewer's files (scripts/web.mjs), and
// models to try in samples/, listed in samples/index.json, which the page offers on its first
// page. Zips it all into dist/geometryview-web.zip, which a release carries and the Pages workflow
// deploys. The models come from the repository's test-models folder, or GEOMETRYVIEW_FIXTURES.
// Each is published under a plain name: the string its header names it by (FILE_NAME), which is
// also its root assembly's name when it has one, becomes the sample's root everywhere it appears.
//
// node scripts/site.mjs [--no-zip]   leaves the site in dist/site either way

import { execFileSync } from "node:child_process";
import { cpSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const launcher = join(dirname(fileURLToPath(import.meta.url)), "..");
const fixtures = process.env.GEOMETRYVIEW_FIXTURES ?? join(launcher, "../test-models");
const site = join(launcher, "dist/site");
const zip = join(launcher, "dist/geometryview-web.zip");

// In the order the page lists them.
const kSamples = [
  { id: "gearbox", root: "planetary_gearbox", name: "Planetary gearbox", from: "models/ple120_gearbox.step", file: "planetary-gearbox.step",
    about: "A planetary gearbox: cut it open to see the gears inside." },
  { id: "drive", root: "geared_drive", name: "Geared drive", from: "models/demo_drive.step", file: "geared-drive.step",
    about: "A compact drive assembly, many parts inside one case." },
  { id: "motor", root: "outrunner_motor", name: "Outrunner motor", from: "models/u8ii_kv100_motor.step", file: "outrunner-motor.step",
    about: "A flat outrunner motor, inside and out." },
  { id: "pack", root: "battery_pack_13s4p", name: "Battery pack, 52 cells", from: "models/pack_13S4P.step", file: "battery-pack.step",
    about: "Cylindrical cells, 13 in series and 4 in parallel, in a case." },
  { id: "flange", root: "gearbox_flange", name: "Gearbox flange", from: "models/gp52c_flange.step", file: "gearbox-flange.step",
    about: "A single machined part." },
  { id: "rack", root: "rack_100k", name: "100,000 boxes", from: "large/rack100k.step", file: "rack-100k.step",
    about: "A synthetic assembly of 100,000 placed parts, to feel the speed." },
];

execFileSync(process.execPath, [join(launcher, "scripts/web.mjs")], { stdio: "inherit" });
rmSync(site, { recursive: true, force: true });
cpSync(join(launcher, "web"), site, { recursive: true });
mkdirSync(join(site, "samples"));
const samples = kSamples.map(({ from, root, ...sample }) => {
  const to = join(site, "samples", sample.file);
  writeFileSync(to, renamed(readFileSync(join(fixtures, from), "latin1"), root), "latin1");
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
