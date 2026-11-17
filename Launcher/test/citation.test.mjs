// How to cite GeometryView is written in several places: CITATION.cff, the README, and the
// instructions AI apps get. They must give the same citation, and the version must be the release's.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { instructions } from "../src/tools.mjs";

const root = new URL("../../", import.meta.url);
const read = (path) => readFileSync(new URL(path, root), "utf8");
const cff = read("CITATION.cff");
// A field's value: the first at that name, in a list item or not.
const field = (name, text = cff) => text.match(new RegExp(`^[\\s-]*${name}: "?([^"\\n]+)"?$`, "m"))?.[1];

const title = field("title");
const version = field("version");
const url = field("repository-code");
const doi = field("doi");

test("the citation file names its title, author, ORCID iD and release", () => {
  assert.equal(title, "GeometryView: A STEP and STL viewer for people and AI agents");
  assert.equal(field("family-names"), "Aalok");
  assert.equal(field("given-names"), "Atharva");
  assert.equal(field("orcid"), "https://orcid.org/0000-0002-1132-9333");
  assert.equal(url, "https://github.com/atharvaaalok/geometryview");
  assert.equal(doi, "10.5281/zenodo.24295273"); // Zenodo's, for every version
  assert.match(field("date-released"), /^\d{4}-\d{2}-\d{2}$/);
  // GitHub's button gives the preferred citation: the same title and author, in one fixed year.
  const preferred = cff.slice(cff.indexOf("preferred-citation:"));
  assert.equal(field("title", preferred), title);
  assert.equal(field("family-names", preferred), "Aalok");
  assert.equal(field("year", preferred), "2026");
  assert.equal(field("doi", preferred), doi);
});

test("its version is the release's", () => {
  const launcher = JSON.parse(read("Launcher/package.json")).version;
  assert.equal(version, launcher);
  assert.equal(JSON.parse(read("Launcher/mcpb/manifest.json")).version, launcher);
  assert.match(read("Launcher/src/mcp.mjs"), new RegExp(`version: "${launcher.replaceAll(".", "\\.")}"`));
});

test("the README gives the same citation, in text and BibTeX", () => {
  const readme = read("README.md");
  const section = readme.slice(readme.indexOf("## Citing GeometryView"));
  assert.ok(section.startsWith("## Citing GeometryView"));
  const text = section.replace(/\n> /g, " ");
  assert.ok(text.includes(`Aalok, A. (2026). *${title}* [Computer software]. Zenodo. https://doi.org/${doi}`));
  const bibtex = section.slice(section.indexOf("```bibtex"), section.indexOf("```", section.indexOf("```bibtex") + 3));
  assert.match(bibtex, /author\s*=\s*\{Aalok, Atharva\}/);
  assert.ok(bibtex.includes(`{{${title}}}`));
  assert.match(bibtex, /year\s*=\s*\{2026\}/);
  assert.ok(bibtex.includes(`{${url}}`));
  assert.match(bibtex, new RegExp(`version\\s*=\\s*\\{${version.replaceAll(".", "\\.")}\\}`));
  assert.ok(bibtex.includes(`{${doi}}`));
});

test("AI apps are told the same citation, to pass on when the work is research", () => {
  assert.ok(instructions.includes(`Aalok, A. (2026). ${title} [Computer software]. Zenodo. https://doi.org/${doi}`));
  assert.match(instructions, /research/);
});
