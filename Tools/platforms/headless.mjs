// Opens a check page in a headless Chrome or Chromium, waits for the report it posts, and prints it.
// For the census and the viewer's scripted checks on machines without a person at them: Linux in
// Docker, Windows on a remote machine, as well as this Mac.
//
//   node Tools/platforms/headless.mjs <url> [--timeout ms] [--shot out.png] [--size WxH] [--phone]
//
// The browser is GEOMETRY_CHROME, or Chrome where it usually lives; GEOMETRY_CHROME_FLAGS adds
// flags, such as the ones that choose a software WebGPU adapter. Exits 0 once a final report
// (one not marked partial) is posted, 1 on a timeout or a report with an error. Zero dependencies.
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const at = args.indexOf(name);
  return at >= 0 ? args[at + 1] : fallback;
};
const url = args[0];
const timeout = Number(option("--timeout", "120000"));
const shot = option("--shot", null);
const [width, height] = option("--size", "1600x1000").split("x").map(Number);
const phone = args.includes("--phone");

const defaults = {
  darwin: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  win32: "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  linux: "/usr/bin/google-chrome",
};
const browser = process.env.GEOMETRY_CHROME ?? defaults[process.platform];
if (!url || !existsSync(browser)) {
  console.error(`usage: headless.mjs <url> [options]; browser ${browser} ${existsSync(browser) ? "" : "not found"}`);
  process.exit(2);
}

const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "geometry-headless-"));
const flags = (process.env.GEOMETRY_CHROME_FLAGS ?? "").split(" ").filter(Boolean);
// GEOMETRY_HEADFUL=1 shows a window instead, as on a virtual X display, where Linux's Chromium can
// present WebGPU and headless cannot.
const headless = process.env.GEOMETRY_HEADFUL === "1" ? [] : ["--headless=new"];
const chrome = spawn(browser, [...headless, "--enable-unsafe-webgpu", `--user-data-dir=${profile}`,
                               `--remote-debugging-port=${port}`, `--window-size=${width},${height}`,
                               "--hide-scrollbars", "--no-first-run", ...flags, "about:blank"],
                     { stdio: "ignore" });
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
// The whole run is bounded, a browser that never answers included.
setTimeout(() => {
  console.error(`no report within ${timeout} ms`);
  finish(1);
}, timeout + 30_000).unref();
const finish = (code) => {
  chrome.kill();
  // Chrome may hold its profile a moment after being asked to end.
  setTimeout(() => {
    try { rmSync(profile, { recursive: true, force: true }); } catch {}
    process.exit(code);
  }, 500);
};

let target;
for (let i = 0; i < 100 && !target; ++i) {
  await sleep(200);
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    target = list.find((t) => t.type === "page");
  } catch {}
}
if (!target) {
  console.error("the browser did not start");
  chrome.kill();
  process.exit(1);
}

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve) => socket.addEventListener("open", resolve));
let id = 0;
const pending = new Map();
let reported;
const done = new Promise((resolve) => { reported = resolve; });
socket.addEventListener("message", (message) => {
  const event = JSON.parse(message.data);
  if (event.id && pending.has(event.id)) {
    pending.get(event.id)(event);
    pending.delete(event.id);
  }
  if (event.method === "Runtime.consoleAPICalled") {
    console.error("console:", event.params.args.map((a) => a.value ?? a.description).join(" "));
  }
  if (event.method === "Runtime.exceptionThrown") {
    const details = event.params.exceptionDetails;
    console.error("exception:", details.exception?.description ?? details.text);
  }
  if (event.method === "Network.requestWillBeSent") {
    const { request } = event.params;
    if (request.method === "POST" && new URL(request.url).pathname === "/report" && request.postData) {
      try {
        const report = JSON.parse(request.postData);
        if (!report.partial) reported(report);
      } catch {}
    }
  }
});
const send = (method, params = {}) => new Promise((resolve) => {
  const i = ++id;
  pending.set(i, resolve);
  socket.send(JSON.stringify({ id: i, method, params }));
});
await send("Runtime.enable");
await send("Network.enable");
await send("Page.enable");
if (phone) {
  await send("Emulation.setUserAgentOverride", {
    userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 " +
               "(KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1",
  });
  await send("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
}
await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: phone ? 3 : 2, mobile: phone });
await send("Page.navigate", { url });

const report = await Promise.race([done, sleep(timeout).then(() => null)]);
if (shot) {
  const picture = await send("Page.captureScreenshot", { format: "png" });
  if (picture.result?.data) writeFileSync(shot, Buffer.from(picture.result.data, "base64"));
}
if (!report) {
  console.error(`no report within ${timeout} ms`);
  finish(1);
} else {
  console.log(JSON.stringify(report, null, 1));
  finish(report.error ? 1 : 0);
}
