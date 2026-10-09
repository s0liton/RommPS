// Drives headless Chrome over its DevTools protocol (no packages needed) to
// screenshot the web UI, or to compare two folders of screenshots.
//
//   node tests/ui_shots.mjs shoot <base-url> <out-dir> <phase>
//   node tests/ui_shots.mjs compare <before-dir> <after-dir> <diff-dir>
//
// Phases match tests/ui-shots.sh: "pair" (unpaired), "setup" (paired, wizard
// steps 3-5) and "app" (every tab). CHROME names the browser binary.
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, readdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const PORT = 9333;
const W = 1600, H = 1000;

const SHOTS = {
  pair: [{ name: "setup-1-server", js: "" }],
  setup: [
    { name: "setup-3-emulators", js: "" , wait: 1500 },
    { name: "setup-4-preview", js: "wzGo(4)", wait: 2000 },
    { name: "setup-5-background", js: "wzGo(5)", wait: 1000 },
  ],
  app: [
    { name: "status", js: "showTab('home')", wait: 1000 },
    { name: "library", js: "showTab('library')", wait: 2000 },
    { name: "library-games", js: "document.querySelector('.plat').click()", wait: 2500 },
    { name: "downloads", js: "showTab('downloads')", wait: 1000 },
    {
      name: "settings",
      js: "showTab('settings'); document.querySelectorAll('[data-collapse].collapsed [data-toggle]').forEach(function (b) { b.click(); })",
      wait: 2500,
    },
    { name: "log", js: "showTab('log')", wait: 1000 },
  ],
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function startChrome() {
  const profile = mkdtempSync(join(tmpdir(), "ui-shots-"));
  const proc = spawn(CHROME, [
    "--headless=new", `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`, `--window-size=${W},${H}`,
    "--hide-scrollbars", "--disable-gpu", "--disable-gpu-rasterization", "--disable-lcd-text", "--no-first-run", "--no-default-browser-check", "--force-device-scale-factor=1", "about:blank",
  ], { stdio: "ignore" });
  for (let i = 0; i < 50; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      if (res.ok) return proc;
    } catch {}
    await sleep(200);
  }
  proc.kill();
  throw new Error("Chrome didn't start");
}

async function session() {
  const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: "PUT" })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, bad) => { ws.onopen = ok; ws.onerror = bad; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) {
      const { ok, bad } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? bad(new Error(msg.error.message)) : ok(msg.result);
    }
  };
  const send = (method, params = {}) => new Promise((ok, bad) => {
    pending.set(++id, { ok, bad });
    ws.send(JSON.stringify({ id, method, params }));
  });
  await send("Page.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  return { send, close: () => ws.close() };
}

async function evaluate(s, expr) {
  const r = await s.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(`${expr}: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
  return r.result.value;
}

// The whole page, not just the window.
async function fullShot(s, file) {
  const m = await s.send("Page.getLayoutMetrics");
  const height = Math.ceil(m.cssContentSize ? m.cssContentSize.height : m.contentSize.height);
  const shot = await s.send("Page.captureScreenshot", {
    format: "png", captureBeyondViewport: true, clip: { x: 0, y: 0, width: W, height, scale: 1 },
  });
  writeFileSync(file, Buffer.from(shot.data, "base64"));
}

async function shoot(base, out, phase) {
  mkdirSync(out, { recursive: true });
  const chrome = await startChrome();
  try {
    const s = await session();
    await s.send("Page.navigate", { url: base });
    await sleep(2500);
    // The same moment for every run: no caret blink or transitions.
    await evaluate(s, "var st = document.createElement('style'); st.textContent = '*,*::before,*::after{transition:none!important;animation:none!important;caret-color:transparent!important}'; document.head.appendChild(st); true");
    for (const shot of SHOTS[phase]) {
      if (shot.js) await evaluate(s, shot.js + "; true");
      await sleep(shot.wait || 800);
      await evaluate(s, "document.activeElement && document.activeElement.blur(); window.scrollTo(0, 0); true");
      await fullShot(s, join(out, shot.name + ".png"));
      console.log("shot", shot.name);
    }
    s.close();
  } finally {
    chrome.kill();
  }
}

// Pixel comparison in a canvas; writes a diff image (changed pixels in red)
// for every pair that differs.
async function compare(before, after, diffDir) {
  mkdirSync(diffDir, { recursive: true });
  const chrome = await startChrome();
  let changed = 0;
  try {
    const s = await session();
    for (const f of readdirSync(before).filter((x) => x.endsWith(".png")).sort()) {
      if (!existsSync(join(after, f))) { console.log(`${f}: missing after`); changed++; continue; }
      const a = readFileSync(join(before, f)).toString("base64"), b = readFileSync(join(after, f)).toString("base64");
      const res = await evaluate(s, `(async () => {
        const load = (d) => new Promise((ok) => { const i = new Image(); i.onload = () => ok(i); i.src = "data:image/png;base64," + d; });
        const [a, b] = await Promise.all([load(${JSON.stringify(a)}), load(${JSON.stringify(b)})]);
        const w = Math.max(a.width, b.width), h = Math.max(a.height, b.height);
        const px = (img) => { const c = new OffscreenCanvas(w, h), x = c.getContext("2d"); x.drawImage(img, 0, 0); return x.getImageData(0, 0, w, h).data; };
        const da = px(a), db = px(b), out = new OffscreenCanvas(w, h), ox = out.getContext("2d"), od = ox.createImageData(w, h);
        let n = 0;
        for (let i = 0; i < da.length; i += 4) {
          const diff = da[i] !== db[i] || da[i + 1] !== db[i + 1] || da[i + 2] !== db[i + 2];
          if (diff) n++;
          od.data[i] = diff ? 255 : da[i] / 3; od.data[i + 1] = diff ? 0 : da[i + 1] / 3; od.data[i + 2] = diff ? 0 : da[i + 2] / 3; od.data[i + 3] = 255;
        }
        ox.putImageData(od, 0, 0);
        const blob = await out.convertToBlob({ type: "image/png" });
        const buf = new Uint8Array(await blob.arrayBuffer());
        let bin = ""; for (let i = 0; i < buf.length; i += 32768) bin += String.fromCharCode.apply(null, buf.subarray(i, i + 32768));
        return { n, size: [a.width, a.height, b.width, b.height], png: n ? btoa(bin) : "" };
      })()`);
      const sizeNote = res.size[0] !== res.size[2] || res.size[1] !== res.size[3] ? ` (size ${res.size[0]}x${res.size[1]} -> ${res.size[2]}x${res.size[3]})` : "";
      console.log(`${f}: ${res.n} pixels differ${sizeNote}`);
      if (res.n) { changed++; writeFileSync(join(diffDir, f), Buffer.from(res.png, "base64")); }
    }
    s.close();
  } finally {
    chrome.kill();
  }
  return changed;
}

const [cmd, ...args] = process.argv.slice(2);
if (cmd === "shoot") await shoot(args[0], args[1], args[2]);
else if (cmd === "compare") process.exit((await compare(args[0], args[1], args[2])) ? 1 : 0);
else { console.error("usage: ui_shots.mjs shoot <url> <out> <phase> | compare <before> <after> <diffs>"); process.exit(2); }
