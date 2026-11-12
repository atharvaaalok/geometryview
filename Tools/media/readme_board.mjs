#!/usr/bin/env node
// Builds a board that shows README drafts as GitHub and npm would: each draft's Markdown rendered
// by GitHub's own Markdown API (so its sanitizing and its HTML are GitHub's), styled with
// github-markdown-css in light and dark, at a desktop column (880 px) and a phone's (360 px), with
// its <picture> sources resolved for each theme, and the page weight each theme loads.
//
//   node Tools/media/readme_board.mjs <board folder>
//
// The board folder holds:
//   board.json        { "title", "intro", "drafts": [{ "id", "name", "idea", "notes" }] }
//   <id>/README.md     the draft, its images as Docs/media/<file> or raw.githubusercontent URLs
//   <id>/npm.md        the npm page's draft (optional)
//   media/<file>       every image the drafts use
// It writes <board folder>/out/index.html and copies the media it uses to out/media. Rendered
// Markdown is cached in <board folder>/.cache by its text, so only changed drafts call GitHub.
// Sending a draft to the Markdown API publishes nothing.

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const board = process.argv[2];
if (!board) {
  process.stderr.write("usage: readme_board.mjs <board folder>\n");
  process.exit(2);
}
const spec = JSON.parse(readFileSync(join(board, "board.json"), "utf8"));
const out = join(board, "out");
mkdirSync(join(out, "media"), { recursive: true });
mkdirSync(join(board, ".cache"), { recursive: true });

// GitHub's Markdown as GitHub renders a repository's README.
function render(markdown) {
  const key = createHash("sha256").update(markdown).digest("hex").slice(0, 16);
  const cached = join(board, ".cache", `${key}.html`);
  if (existsSync(cached)) return readFileSync(cached, "utf8");
  const html = execFileSync("gh", ["api", "markdown", "-f", "mode=markdown", "-f",
                                   "context=atharvaaalok/geometryview", "-F", "text=@-"],
                            { input: markdown, encoding: "utf8" });
  writeFileSync(cached, html);
  return html;
}

// The stylesheets, fetched once: github-markdown-css, MIT.
async function stylesheet(theme) {
  const file = join(board, ".cache", `github-markdown-${theme}.css`);
  if (!existsSync(file)) {
    const url = `https://cdn.jsdelivr.net/npm/github-markdown-css@5.8.1/github-markdown-${theme}.css`;
    const response = await fetch(url);
    if (!response.ok) throw new Error(`${url}: ${response.status}`);
    writeFileSync(file, await response.text());
  }
  // Scoped to its own panes, so both themes live on one page.
  return readFileSync(file, "utf8").replaceAll(".markdown-body", `.gh-${theme} .markdown-body`);
}

// Image references in drafts point at the repository; on the board they point at out/media.
const media = new Map(); // file name -> bytes
function localize(html) {
  return html.replace(/(src|srcset)="([^"]+)"/g, (whole, attribute, value) => {
    const match = /(?:^|\/)Docs\/media\/([^"/?#]+)$/.exec(value);
    if (!match) return whole;
    const file = match[1];
    const source = join(board, "media", file);
    if (!existsSync(source)) {
      process.stderr.write(`missing media: ${file}\n`);
      return whole;
    }
    if (!media.has(file)) {
      copyFileSync(source, join(out, "media", file));
      media.set(file, statSync(source).size);
    }
    return `${attribute}="media/${file}"`;
  });
}

// One theme's view of a draft: <picture> resolved as that theme's browser would.
function themed(html, theme) {
  return html.replace(/<picture>([\s\S]*?)<\/picture>/g, (whole, inside) => {
    const dark = /<source[^>]*media="\(prefers-color-scheme: dark\)"[^>]*srcset="([^"]+)"/.exec(inside);
    const light = /<source[^>]*media="\(prefers-color-scheme: light\)"[^>]*srcset="([^"]+)"/.exec(inside);
    let body = inside.replace(/<source[^>]*>/g, "");
    const pick = theme === "dark" ? dark : light;
    if (pick) body = body.replace(/(<img[^>]*?)src="[^"]+"/, `$1src="${pick[1]}"`);
    return body;
  });
}

// What each theme loads: every image it shows, once.
function weight(html) {
  const files = new Set([...html.matchAll(/src="media\/([^"]+)"/g)].map((m) => m[1]));
  let bytes = 0;
  for (const file of files) bytes += media.get(file) ?? 0;
  return bytes;
}

// What GitHub's sanitizer dropped: tags and attributes in the draft that its HTML lacks.
function dropped(markdown, html) {
  const out = [];
  for (const [tag] of markdown.matchAll(/<(video|svg|style|script|iframe|div style|span style)\b/g)) {
    if (!html.includes(`<${tag.split(" ")[0]}`)) out.push(`<${tag}>`);
  }
  if (/\sstyle="/.test(markdown) && !/\sstyle="(?!max-width: 100%)/.test(html)) out.push("style=");
  return [...new Set(out)];
}

const drafts = [];
for (const draft of spec.drafts) {
  const folder = join(board, draft.id);
  const readme = readFileSync(join(folder, "README.md"), "utf8");
  const html = localize(render(readme));
  const npmPath = join(folder, "npm.md");
  const npm = existsSync(npmPath) ? localize(render(readFileSync(npmPath, "utf8"))) : null;
  const light = themed(html, "light");
  const dark = themed(html, "dark");
  drafts.push({
    ...draft,
    light, dark, npm: npm && themed(npm, "light"),
    weight: { light: weight(light), dark: weight(dark), npm: npm ? weight(themed(npm, "light")) : 0 },
    dropped: dropped(readme, html),
    lines: readme.split("\n").length,
  });
}

const css = (await stylesheet("light")) + "\n" + (await stylesheet("dark"));
const template = readFileSync(new URL("./readme_board.html", import.meta.url), "utf8");
const page = template
  .replace("/*GITHUB_CSS*/", () => css)
  .replace("\"__BOARD__\"", () => JSON.stringify({ title: spec.title, intro: spec.intro, drafts })
    .replaceAll("</", "<\\/"));
writeFileSync(join(out, "index.html"), page);
const total = [...media.values()].reduce((a, b) => a + b, 0);
process.stdout.write(`${join(out, "index.html")}: ${drafts.length} drafts, ${media.size} media files, ` +
                     `${(total / 1e6).toFixed(1)} MB\n`);
for (const d of drafts) {
  process.stdout.write(`  ${d.id} ${d.name}: light ${(d.weight.light / 1e6).toFixed(2)} MB, ` +
                       `dark ${(d.weight.dark / 1e6).toFixed(2)} MB` +
                       (d.dropped.length ? `, dropped ${d.dropped.join(" ")}` : "") + "\n");
}
