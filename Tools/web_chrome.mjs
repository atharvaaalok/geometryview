#!/usr/bin/env node
// Opens the web viewer in headless Chrome at a window's size and reports where its chrome is, for
// comparing with the Mac app's --chrome-layout report, and saves a screenshot.
//
//   node Tools/web_chrome.mjs <viewer url> <out-prefix> [width height scale]
//
// Writes <out-prefix>.json (each named element's box in CSS pixels from the page's top left) and
// <out-prefix>.png. Waits for the scene to be ready, or for the empty state if no model is asked
// for. Chrome is GEOMETRYVIEW_CHROME, or Google Chrome's usual place on a Mac.

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [url, out, width = "1440", height = "900", scale = "2", device = "desktop"] = process.argv.slice(2);
// "phone" sends an iPhone's user agent, as the viewer tells a phone by it.
const kPhoneAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 " +
                    "(KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1";
if (!url || !out) {
  process.stderr.write("usage: web_chrome.mjs <viewer url> <out-prefix> [width height scale]\n");
  process.exit(2);
}
const chromePath = process.env.GEOMETRYVIEW_CHROME ??
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "geometryview-chrome-"));
const chrome = spawn(chromePath, ["--headless=new", `--remote-debugging-port=${port}`,
  `--user-data-dir=${profile}`, "--no-first-run", "--enable-unsafe-webgpu",
  `--window-size=${width},${height}`, "about:blank"], { stdio: "ignore" });

const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

// The elements compared, by name: one box, or every match for a list.
const kElements = {
  "masthead.title": ".masthead .title",
  "masthead.subtitle": ".masthead .subtitle",
  "masthead.close": ".masthead .title-row .pill",
  "structure.card": ".card.structure:not(.library):not([hidden])",
  "structure.switcher": ".card.structure:not(.library):not([hidden]) .segmented",
  "structure.segment": [".card.structure:not(.library):not([hidden]) .segmented button"],
  "structure.search": ".card.structure:not(.library):not([hidden]) .searchbox",
  "library.card": ".card.library:not([hidden])",
  "library.switcher": ".card.library:not([hidden]) .segmented",
  "library.button": [".card.library:not([hidden]) .header .pill"],
  "tree.row": [".tree .row"],
  "tree.chevron": [".tree .row button.chevron"],
  "tree.glyph": [".tree .row .glyph"],
  "tree.swatch": [".tree .row .swatch"],
  "tree.name": [".tree .row .name"],
  "tree.count": [".tree .row .count"],
  "toolbar.card": ".toolbar",
  "toolbar.item": [".toolbar > *:not([hidden])"],
  "status.card": ".card.status:not([hidden])",
  "status.heading": ".card.status:not([hidden]) h2",
  "status.body": ".card.status:not([hidden]) p",
  "status.button": [".card.status:not([hidden]) button"],
};

function measure(elements) {
  const box = (e) => {
    const r = e.getBoundingClientRect();
    return { x: r.x, y: r.y, w: r.width, h: r.height };
  };
  const shown = (e) => e.getClientRects().length > 0;
  const report = {};
  // The toolbar's tools that show, by name, in order.
  report["toolbar.tools"] = [...document.querySelectorAll(".toolbar [data-tool]")].filter(shown)
    .map((e) => e.dataset.tool);
  for (const [name, selector] of Object.entries(elements)) {
    if (Array.isArray(selector)) {
      report[name] = [...document.querySelectorAll(selector[0])].filter(shown).slice(0, 8).map(box);
    } else {
      const e = document.querySelector(selector);
      if (e && shown(e)) report[name] = box(e);
    }
  }
  return report;
}

try {
  let target;
  for (let i = 0; i < 50 && !target; ++i) {
    await sleep(100);
    target = await fetch(`http://127.0.0.1:${port}/json/list`).then((r) => r.json())
      .then((list) => list.find((t) => t.type === "page")).catch(() => null);
  }
  if (!target) throw new Error("Chrome did not start.");
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((done, fail) => { socket.onopen = done; socket.onerror = fail; });
  let next = 1;
  const pending = new Map();
  const pageErrors = [];
  socket.onmessage = ({ data }) => {
    const message = JSON.parse(data);
    if (message.method === "Runtime.exceptionThrown") {
      pageErrors.push(message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text);
    } else if (message.method === "Runtime.consoleAPICalled" && message.params.type === "error") {
      pageErrors.push(message.params.args.map((a) => a.value ?? a.description).join(" "));
    }
    pending.get(message.id)?.(message);
    pending.delete(message.id);
  };
  const send = (method, params = {}) => new Promise((done) => {
    const id = next++;
    pending.set(id, done);
    socket.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async (expression) =>
    (await send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true })).result?.result?.value;

  await send("Runtime.enable");
  if (device === "phone") await send("Emulation.setUserAgentOverride", { userAgent: kPhoneAgent });
  await send("Emulation.setDeviceMetricsOverride", { width: +width, height: +height,
                                                     deviceScaleFactor: +scale, mobile: device === "phone" });
  await send("Page.navigate", { url });
  const wantsModel = /[?&](model|import)=/.test(url);
  for (let i = 0; i < 600; ++i) {
    await sleep(100);
    const ready = await evaluate(wantsModel
      ? `(() => { const s = document.querySelector(".masthead .subtitle"); return !!s && s.textContent.includes("bodies") && !document.querySelector(".masthead .progress:not([hidden])"); })()`
      : `!!document.querySelector(".card.status:not([hidden])")`);
    if (ready) break;
  }
  await sleep(1500); // the view settles, and fonts are in
  // GEOMETRYVIEW_CLICK clicks an element first, such as a menu's button, to picture what it opens.
  if (process.env.GEOMETRYVIEW_CLICK) {
    await evaluate(`document.querySelector(${JSON.stringify(process.env.GEOMETRYVIEW_CLICK)})?.click()`);
    await sleep(300);
  }
  const report = await evaluate(`(${measure})(${JSON.stringify(kElements)})`);
  if (pageErrors.length) report.errors = pageErrors;
  writeFileSync(`${out}.json`, JSON.stringify(report, null, 1));
  const shot = await send("Page.captureScreenshot", { format: "png" });
  writeFileSync(`${out}.png`, Buffer.from(shot.result.data, "base64"));
  socket.close();
} finally {
  chrome.kill();
  await sleep(200);
  rmSync(profile, { recursive: true, force: true });
}
