#!/usr/bin/env node
// Keeps the npm page's README (Launcher/README.md) sound. npm shows a README's pictures only from
// absolute URLs, and a page published once is never updated, so its pictures are pinned to the
// release's tag, where they can't change or vanish.
//
//   node Launcher/scripts/readme.mjs            pins every picture to v<package.json's version>
//   node Launcher/scripts/readme.mjs --ref v0.4.0
//   node Launcher/scripts/readme.mjs --check    fails on a picture pinned elsewhere, a relative link,
//                                               a <picture>, or any URL that doesn't answer
//
// `npm publish` runs --check first (prepublishOnly), so the tag must be pushed before publishing.

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const readmePath = join(here, "../README.md");
const { version } = JSON.parse(readFileSync(join(here, "../package.json"), "utf8"));
const args = process.argv.slice(2);
const ref = args.includes("--ref") ? args[args.indexOf("--ref") + 1] : `v${version}`;
const pinned = /https:\/\/raw\.githubusercontent\.com\/atharvaaalok\/geometryview\/([^/]+)\//g;

let text = readFileSync(readmePath, "utf8");

if (!args.includes("--check")) {
  const before = text;
  text = text.replace(pinned, `https://raw.githubusercontent.com/atharvaaalok/geometryview/${ref}/`);
  if (text !== before) writeFileSync(readmePath, text);
  process.stdout.write(`${readmePath}: pictures pinned to ${ref}${text === before ? " already" : ""}\n`);
  process.exit(0);
}

const problems = [];
for (const [, at] of text.matchAll(pinned)) {
  if (at !== ref) problems.push(`a picture is pinned to ${at}, not ${ref}: run node Launcher/scripts/readme.mjs`);
}
if (/<picture[\s>]/i.test(text)) problems.push("npm ignores <picture>: use one <img>");
const urls = new Set();
for (const [, url] of text.matchAll(/\]\(([^)\s]+)\)/g)) urls.add(url);
for (const [, url] of text.matchAll(/(?:src|href)="([^"]+)"/g)) urls.add(url);
for (const url of urls) {
  if (!/^https?:\/\//.test(url)) {
    problems.push(`${url}: relative links and anchors don't work on npm`);
    continue;
  }
  // GitHub answers HEAD for pages and raw files alike; a redirect to a release asset counts.
  const response = await fetch(url, { method: "HEAD", redirect: "follow" }).catch((error) => ({ error }));
  if (response.error || !response.ok) {
    problems.push(`${url}: ${response.error?.message ?? response.status}`);
  }
}
for (const p of [...new Set(problems)]) process.stderr.write(`${p}\n`);
process.stdout.write(`${readmePath}: ${urls.size} links, ${problems.length ? "not ready" : "all answer"}\n`);
process.exit(problems.length ? 1 : 0);
