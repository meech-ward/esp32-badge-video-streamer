// Check the unmodified badge viewer in the existing Badgeware/PicoVector runtime.
// Only networking is replaced by tests/emulator_support.py; fixtures and test
// credentials live in WASM RAM. No real Wi-Fi settings are read.
// BADGE_SOURCE=/path/to/badge node tools/check_emulator.mjs
// This optional check needs a prebuilt compatible badge playground.
import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { deflateSync } from "node:zlib";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
if (!process.env.BADGE_SOURCE) {
  console.error("Set BADGE_SOURCE to a badge checkout with its Badgeware v3.0.2 playground built.");
  process.exit(1);
}
const badge = path.resolve(process.env.BADGE_SOURCE);
const runtime = path.join(badge, "playground/public/runtime");
const generated = path.join(badge, "playground/public/generated");
const compat = path.join(badge, "playground/src/compat/badgeware-v3.0.2");
const appSource = path.join(root, "tufty/video_streamer");
const APP = "/system/apps/video_streamer";
const output = path.join(root, "receipts/emulator");
for (const required of [
  path.join(runtime, "micropython.mjs"), path.join(runtime, "micropython.wasm"),
  path.join(runtime, "provenance.json"), path.join(compat, "pg_runtime.py"),
  path.join(generated, "badgeware/files"), path.join(generated, "romfs/fonts"),
]) {
  if (!fs.existsSync(required)) {
    console.error(`Missing playground prerequisite: ${required}. Build the playground in BADGE_SOURCE first.`);
    process.exit(1);
  }
}
fs.mkdirSync(output, { recursive: true });
const sha = value => createHash("sha256").update(value).digest("hex");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const provenance = JSON.parse(fs.readFileSync(path.join(runtime, "provenance.json")));
for (const [name, info] of Object.entries(provenance.artifacts)) {
  assert.equal(sha(fs.readFileSync(path.join(runtime, name))), info.sha256, `runtime artifact ${name} changed`);
}
// Use test-only credentials; never read or mount the real local configuration.
const SKIP = new Set(["__pycache__", "wifi_config.py"]);

// Minimal PNG writer (stdlib only); raw RGBA straight from the rasterizer.
function png(rgba, width, height) {
  const crc = bytes => {
    let value = 0xffffffff;
    for (const byte of bytes) {
      value ^= byte;
      for (let bit = 0; bit < 8; ++bit) value = value & 1 ? (value >>> 1) ^ 0xedb88320 : value >>> 1;
    }
    return (value ^ 0xffffffff) >>> 0;
  };
  const chunk = (name, bytes) => {
    const type = Buffer.from(name), head = Buffer.alloc(4), tail = Buffer.alloc(4);
    head.writeUInt32BE(bytes.length); tail.writeUInt32BE(crc(Buffer.concat([type, bytes])));
    return Buffer.concat([head, type, bytes, tail]);
  };
  const header = Buffer.alloc(13); header.writeUInt32BE(width); header.writeUInt32BE(height, 4);
  header[8] = 8; header[9] = 6;
  const stride = width * 4, scanlines = Buffer.alloc(height * (stride + 1));
  for (let y = 0; y < height; ++y) rgba.copy(scanlines, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", header),
    chunk("IDAT", deflateSync(scanlines)), chunk("IEND", Buffer.alloc(0))]);
}

async function session(label) {
  const { loadMicroPython } = await import(pathToFileURL(path.join(runtime, "micropython.mjs")) + `?${label}`);
  const logs = [], events = [], mounted = {};
  let result = null, frame = null, buttons = "", payload = "{}", probe = null;
  const mp = await loadMicroPython({ heapsize: 8 * 1024 * 1024,
    stdout: line => logs.push(line), stderr: line => logs.push(line) });
  mp.registerJsModule("pg_host", {
    buttons_down: () => buttons, button_is_down: name => buttons.split(",").includes(name),
    peripheral_float: name => ({ battery_voltage: 4.05, usb_connected: 1,
      charging: 0, wake_reason: 255, light_level: 30000 }[name] || 0),
    emit_event: (name, data) => events.push({ name, data: JSON.parse(data) }),
    present_frame: (width, height) => { frame = { width, height }; },
    get_config: () => "{}", network_config: () => "{}", step_payload: () => payload,
    step_result: value => { result = JSON.parse(value); },
    emulator_probe: value => { probe = JSON.parse(value); },
  });
  const mkdir = dest => {
    if (dest === "/" || mp.FS.analyzePath(dest).exists) return;
    mkdir(path.posix.dirname(dest)); mp.FS.mkdir(dest);
  };
  const mount = (source, dest) => {
    mkdir(dest);
    for (const entry of fs.readdirSync(source, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      if (SKIP.has(entry.name) || entry.name.endsWith(".example.py")) continue;
      const src = path.join(source, entry.name), dst = dest + "/" + entry.name;
      if (entry.isDirectory()) { mount(src, dst); continue; }
      const bytes = fs.readFileSync(src);
      mp.FS.writeFile(dst, bytes);
      mounted[dst] = sha(bytes);
    }
  };
  mount(compat, "/playground/compat");
  mount(path.join(generated, "badgeware/files"), "/playground/compat/badgeware");
  mount(path.join(generated, "romfs/fonts"), "/rom/fonts");
  mount(appSource, APP);
  mount(path.join(root, "tests/fixtures"), "/fixtures");
  mp.FS.writeFile(APP + "/wifi_config.py", 'SSID = "test-camera"\nPASSWORD = "test-only-value"\n');
  mp.FS.writeFile("/playground/compat/emulator_support.py",
    fs.readFileSync(path.join(root, "tests/emulator_support.py")));
  mkdir("/state");
  mp.runPython("import sys\nsys.path[:0] = ['/playground/compat', '" + APP + "']\n" +
    "import emulator_support\nemulator_support.install()\n" +
    "import pg_runtime\npg_runtime.init()\n" + `pg_runtime.launch_app('${APP}')`);
  assert.equal(result?.error, null, JSON.stringify(result));
  assert.equal(result?.run_captured, true);

  const W = 160, H = 120;
  const step = (extra = {}) => {
    payload = JSON.stringify(extra);
    mp.runPython("pg_runtime.step_frame()");
    payload = "{}";
    return result;
  };
  const inspect = () => {
    mp.runPython(`import json, pg_host\npg_host.emulator_probe(json.dumps(sys.modules['${APP}'].debug_state()))`);
    return probe;
  };
  // Real time drives the test camera's 25 fps pacing.
  const run = async (ms, held = "") => {
    buttons = held;
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const r = step();
      assert.equal(r.error, null, JSON.stringify(r));
      assert.equal(r.finished, false, JSON.stringify(r));
      assert.deepEqual(frame, { width: W, height: H });
      await sleep(8);
    }
    buttons = "";
  };
  const snapshots = [];
  const capture = (name, { status = false } = {}) => {
    const state = inspect();
    const rgba = Buffer.from(mp.FS.readFile("/playground/frame.rgba"));
    assert.equal(rgba.length, W * H * 4);
    const px = (x, y) => { const i = (y * W + x) * 4; return [rgba[i], rgba[i + 1], rgba[i + 2]]; };
    if (status) {
      const colors = new Set();
      for (let y = 0; y < H; ++y) for (let x = 0; x < W; ++x) colors.add(px(x, y).join());
      assert.ok(colors.size >= 3, `${label}/${name}: blank status screen`);
      const bytes = png(rgba, W, H), file = `${label}-${name}.png`;
      fs.writeFileSync(path.join(output, file), bytes);
      snapshots.push({ name: `${label}-${name}`, file, png_sha256: sha(bytes), colors: colors.size, state });
      return state;
    }
    // The fixture JPEG's magenta marker must be
    // where the decoder put it, i.e. the frame really came through load_into.
    const marker = px(Math.floor(W * 0.35), Math.floor(H * 0.45));
    const near = (a, b) => a.every((v, i) => Math.abs(v - b[i]) <= 24);
    assert.ok(near(marker, [230, 40, 200]), `${label}/${name}: marker pixel ${marker}`);
    // Frame area (between the overlay strips) is not blank.
    const colors = new Set();
    for (let y = 14; y < H - 14; ++y) for (let x = 2; x < W - 2; ++x) colors.add(px(x, y).join());
    assert.ok(colors.size >= 8, `${label}/${name}: frame area looks blank (${colors.size} colours)`);
    const bytes = png(rgba, W, H), file = `${label}-${name}.png`;
    fs.writeFileSync(path.join(output, file), bytes);
    snapshots.push({ name: `${label}-${name}`, file, png_sha256: sha(bytes), marker, colors: colors.size, state });
    return state;
  };

  // The test socket refuses its first two sends, like a connect in progress.
  assert.equal(step().error, null);
  let s = capture("connecting", { status: true });
  assert.equal(s.state, "connecting");
  assert.equal(s.shown, 0);

  await run(1500);
  s = capture("hud-on");
  assert.ok(s.shown >= 10 && s.decode_errors === 0 && s.errors === 0, JSON.stringify(s));
  assert.equal(s.width, W);
  assert.ok(s.statuses >= 1);
  assert.equal(s.hud, true);
  assert.ok(s.fps10 > 0);
  assert.match(s.top, /fps n\d+ d\d+/);
  assert.match(s.bottom, /s\d+ g\d+ a\d+/);

  // B toggles once on a press, including when held across many frames.
  await run(500, "B");
  assert.equal(inspect().hud, false);
  await run(100);
  s = capture("hud-off");
  assert.equal(s.hud, false);
  const rgba = Buffer.from(mp.FS.readFile("/playground/frame.rgba"));
  assert.ok(rgba[0] + rgba[1] + rgba[2] > 30, "HUD black strip remained after hiding");
  await run(100, "A,C,UP,DOWN");
  assert.equal(inspect().hud, false, "other buttons changed the viewer");
  await run(100);
  await run(100, "B");
  await run(300);
  s = capture("hud-restored");
  assert.equal(s.hud, true);
  const before = inspect();
  const r = step({ home: true });
  assert.equal(r.finished, true);
  assert.equal(r.reason, "reset");
  assert.ok(!logs.some(line => line.includes("ERROR")), logs.join("\n"));
  assert.equal(events.filter(event => event.name === "fatal_error").length, 0);
  return { label, snapshots, final_state: before,
    logs: logs.filter(line => !line.startsWith("- playground shim")),
    events: events.map(event => event.name),
    mounted_sha256: Object.fromEntries(Object.entries(mounted).filter(([name]) => name.startsWith(APP))) };
}

const sessions = [await session("lores")];
const report = {
  generated_at: new Date().toISOString(),
  passed: true,
  scope: "Unmodified viewer in pinned Badgeware v3.0.2/PicoVector WASM: stream parsing, " +
    "direct RAM JPEG decode, FPS HUD, B show/hide, ignored unused buttons, GC and HOME exit",
  not_tested: ["real sockets/lwIP", "CYW43 Wi-Fi", "ESP hardware", "device timing and battery"],
  runtime_provenance: { micropython: provenance.micropython?.commit,
    picovector: provenance.picovector?.commit, tufty2350: provenance.tufty2350?.commit },
  harness_sha256: sha(fs.readFileSync(fileURLToPath(import.meta.url))),
  sessions,
};
fs.writeFileSync(path.join(output, "report.json"), JSON.stringify(report, null, 2) + "\n");
console.log(JSON.stringify({ passed: true,
  snapshots: sessions.flatMap(session => session.snapshots.map(item => item.file)),
  viewer: sessions[0].final_state }, null, 1));
