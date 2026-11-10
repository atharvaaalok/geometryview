#!/usr/bin/env node
// Drives the web viewer in headless Chrome through a list of steps, for checks and for the
// README's pictures. Steps are JSON, in a file or on the command line:
//
//   node Tools/media/page.mjs steps.json      or      node Tools/media/page.mjs '[{...}, ...]'
//
//   { "size": [w, h, scale], "phone": true }   the window, and an iPhone's user agent (first step)
//   { "go": url }                              navigate
//   { "until": expression, "timeout": s }      wait for an expression to be true (default 120 s)
//   { "wait": ms }                             wait
//   { "key": "t" }                             press and release a key
//   { "eval": expression, "as": name }         evaluate, and keep the value in the report as name
//   { "shot": file.png }                       screenshot
//   { "shots": prefix, "every": ms, "until": expression, "timeout": s }
//                                              screenshots every ms until the expression is true
//
// Prints the report as JSON: the kept values, timings and any page errors. Chrome is
// GEOMETRYVIEW_CHROME, or Google Chrome's usual place on a Mac.

import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const kPhoneAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 " +
                    "(KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1";
const argument = process.argv[2];
if (!argument) {
  process.stderr.write("usage: page.mjs <steps.json | '[steps]'>\n");
  process.exit(2);
}
const steps = JSON.parse(existsSync(argument) ? readFileSync(argument, "utf8") : argument);
const first = steps[0]?.size ? steps[0] : { size: [1440, 900, 2] };
const [width, height, scale = 2] = first.size;

const chromePath = process.env.GEOMETRYVIEW_CHROME ??
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "geometryview-page-"));
const chrome = spawn(chromePath, ["--headless=new", `--remote-debugging-port=${port}`,
  `--user-data-dir=${profile}`, "--no-first-run", "--enable-unsafe-webgpu",
  `--window-size=${width},${height}`, "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

const report = { errors: [] };
const started = performance.now();
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
  socket.onmessage = ({ data }) => {
    const message = JSON.parse(data);
    if (message.method === "Runtime.exceptionThrown") {
      const details = message.params.exceptionDetails;
      report.errors.push(details.exception?.description ?? details.text);
    } else if (message.method === "Runtime.consoleAPICalled" && message.params.type === "error") {
      report.errors.push(message.params.args.map((a) => a.value ?? a.description).join(" "));
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
  const shoot = async (file) => {
    const shot = await send("Page.captureScreenshot", { format: "png" });
    writeFileSync(file, Buffer.from(shot.result.data, "base64"));
  };

  await send("Runtime.enable");
  if (first.phone) await send("Emulation.setUserAgentOverride", { userAgent: kPhoneAgent });
  await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: scale,
                                                     mobile: !!first.phone });
  for (const step of steps) {
    const at = () => Math.round(performance.now() - started);
    if (step.go) {
      await send("Page.navigate", { url: step.go });
    } else if (step.shots) {
      let count = 0;
      const deadline = performance.now() + (step.timeout ?? 120) * 1000;
      for (;;) {
        const t = performance.now();
        await shoot(`${step.shots}${String(count++).padStart(4, "0")}.png`);
        if (await evaluate(step.until) || performance.now() > deadline) break;
        await sleep(Math.max(0, step.every - (performance.now() - t)));
      }
      report[`${step.shots}:frames`] = count;
    } else if (step.until) {
      const deadline = performance.now() + (step.timeout ?? 120) * 1000;
      while (!(await evaluate(step.until).catch(() => false))) {
        if (performance.now() > deadline) throw new Error(`timed out waiting for ${step.until}`);
        await sleep(100);
      }
      if (step.as) report[step.as] = at();
    } else if (step.wait) {
      await sleep(step.wait);
    } else if (step.key) {
      const code = `Key${step.key.toUpperCase()}`;
      await send("Input.dispatchKeyEvent", { type: "keyDown", key: step.key, code, text: step.key });
      await send("Input.dispatchKeyEvent", { type: "keyUp", key: step.key, code });
    } else if (step.eval) {
      const value = await evaluate(step.eval);
      if (step.as) report[step.as] = value;
    } else if (step.shot) {
      await shoot(step.shot);
    }
  }
  socket.close();
} catch (error) {
  report.failed = error.message;
} finally {
  chrome.kill();
  await sleep(200);
  rmSync(profile, { recursive: true, force: true });
}
console.log(JSON.stringify(report, null, 1));
process.exit(report.failed ? 1 : 0);
