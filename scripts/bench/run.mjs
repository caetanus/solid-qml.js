#!/usr/bin/env node
// Native-vs-web rows benchmark runner (release-plan M10). Methodology: docs/performance.md.
//
// Runs the SAME Solid component (examples/bench-rows.tsx, examples/bench-mem.tsx) natively through
// solid-qml-loader and in headless Chromium (production vite build), and writes raw results with
// environment metadata to bench/results/<date>-<host>.json plus CSVs next to it.
//
// Usage: node --import tsx scripts/bench/run.mjs [options]
//   --build <dir>        meson build dir with solid-qml-loader (default: build-bench)
//   --sizes 1000,10000   rows created by "create"
//   --rounds 11          rounds per process; round 0 is warm-up (kept in the raw data, not in medians)
//   --reps 2             processes per configuration (samples per op = reps × (rounds − 1))
//   --mem-reps 10        processes per memory/startup configuration (0 = skip)
//   --targets native,web
//   --max-wait-min 60    load gate: give up waiting for a quiet machine after this (then run, flagged)
//   --no-gate            skip the load gate (still records load)
//   --out bench/results
//   --tag <name>         suffix for the output file names
//   --only <list>        only these configurations: target:scroll:n[:key], comma-separated
// Native runs use QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=rhi, QSG_RHI_BACKEND=null (no software
// rasterizer in the timings; nothing is drawn — GPU presentation is NOT measured by this runner).
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const args = parseArgs(process.argv.slice(2));
const BUILD = path.resolve(ROOT, args.build ?? "build-bench");
const SIZES = (args.sizes ?? "1000,10000").split(",").map(Number);
const ROUNDS = Number(args.rounds ?? 11);
const REPS = Number(args.reps ?? 2);
const MEM_REPS = Number(args["mem-reps"] ?? 10);
const TARGETS = (args.targets ?? "native,web").split(",");
const MAX_WAIT_MIN = Number(args["max-wait-min"] ?? 60);
const GATE = !args["no-gate"];
const OUT = path.resolve(ROOT, args.out ?? "bench/results");
const TMP = process.env.TMPDIR ?? path.join(ROOT, ".scratch/tmp");
const WORK = fs.mkdtempSync(path.join(TMP, "bench-"));
const LOADER = path.join(BUILD, "solid-qml-loader");
const CHROMIUM = args.chromium ?? "chromium";
const RUN_TIMEOUT_MS = 15 * 60 * 1000;
const MEM_SETTLE_MS = 2500; // after "MEM rows" (logged 500 ms after mounting): 3 s total, steady state
const OPS = ["create1k", "update10th", "swap", "remove1", "append1k", "clear"];

function parseArgs(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const k = a.slice(2);
    if (k.startsWith("no-")) o[k] = true;
    else o[k] = argv[++i];
  }
  return o;
}

const sh = (cmd, a = []) => {
  try {
    return execFileSync(cmd, a, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim();
  } catch {
    return null;
  }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...m) => console.error(`[bench ${new Date().toISOString().slice(11, 19)}]`, ...m);

// ── environment ────────────────────────────────────────────────────────────────────────────────
function environment() {
  const hasBuild = fs.existsSync(path.join(BUILD, "meson-info/intro-buildoptions.json"));
  const lib = hasBuild ? fs.readdirSync(BUILD).find((f) => /^libsolidqml\.so\.\d+\.\d+\.\d+$/.test(f)) : null;
  const opts = hasBuild ? JSON.parse(fs.readFileSync(path.join(BUILD, "meson-info/intro-buildoptions.json"), "utf8")) : [];
  const opt = (n) => opts.find((o) => o.name === n)?.value;
  const osRelease = Object.fromEntries(
    fs.readFileSync("/etc/os-release", "utf8").split("\n").filter(Boolean)
      .map((l) => l.split("=")).map(([k, v]) => [k, (v ?? "").replace(/^"|"$/g, "")]),
  );
  const ninjaDry = hasBuild ? sh("ninja", ["-C", BUILD, "-n"]) ?? "" : "";
  return {
    date: new Date().toISOString(),
    host: os.hostname(),
    kernel: os.release(),
    os: osRelease.PRETTY_NAME,
    cpu: os.cpus()[0]?.model,
    cpuCount: os.cpus().length,
    cpuGovernor: sh("cat", ["/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"]),
    memTotalMiB: Math.round(os.totalmem() / 2 ** 20),
    node: process.version,
    qt: sh("pkg-config", ["--modversion", "Qt6Core"]),
    chromium: sh(CHROMIUM, ["--version"]),
    gitSha: sh("git", ["-C", ROOT, "rev-parse", "HEAD"]),
    gitDirty: (sh("git", ["-C", ROOT, "status", "--porcelain", "--untracked-files=no"]) ?? "") !== "",
    engineSha: sh("git", ["-C", path.join(ROOT, "vendor/qml-css-engine"), "rev-parse", "HEAD"]),
    build: {
      dir: path.relative(ROOT, BUILD),
      buildtype: opt("buildtype"),
      optimization: opt("optimization"),
      debug: opt("debug"),
      qmlDebug: opt("qml_debug"),
      upToDate: /no work to do/.test(ninjaDry),
      libsolidqml: lib,
      libSha256: lib ? sh("sha256sum", [path.join(BUILD, lib)])?.slice(0, 16) : null,
    },
    native: {
      env: { QT_QPA_PLATFORM: "offscreen", QT_QUICK_BACKEND: "rhi", QSG_RHI_BACKEND: "null" },
      window: "800x900",
    },
    web: {
      flags: chromeFlags("<profile>").filter((f) => !f.startsWith("--user-data-dir")),
      build: "vite build (production)",
    },
    params: { sizes: SIZES, rounds: ROUNDS, warmupRounds: 1, reps: REPS, memReps: MEM_REPS, memRows: 2000 },
  };
}

// ── load gate ──────────────────────────────────────────────────────────────────────────────────
const BUILD_PROCS = /^(cc1plus|ninja|podman|ld|ld\.lld|ldc2|rustc)$/;
function buildProcs() {
  const out = [];
  for (const pid of fs.readdirSync("/proc").filter((d) => /^\d+$/.test(d))) {
    try {
      const comm = fs.readFileSync(`/proc/${pid}/comm`, "utf8").trim();
      if (BUILD_PROCS.test(comm)) out.push(`${comm}:${pid}`);
    } catch {}
  }
  return out;
}
function topCpu(n = 6) {
  // Instantaneous: second iteration of top (the first one is the since-boot average).
  const txt = sh("top", ["-b", "-c", "-n", "2", "-d", "1", "-w", "400"]) ?? "";
  const blocks = txt.split(/^top - /m);
  const last = blocks[blocks.length - 1].split("\n");
  const hdr = last.findIndex((l) => /^\s*PID\s/.test(l));
  return last.slice(hdr + 1).filter((l) => l.trim()).slice(0, n).map((l) => {
    const c = l.trim().split(/\s+/);
    return { pid: Number(c[0]), cpu: Number(c[8].replace(",", ".")), command: c.slice(11).join(" ").slice(0, 160) };
  });
}
function loadSnapshot() {
  const [l1, l5, l15] = os.loadavg();
  const avail = Number(/MemAvailable:\s+(\d+)/.exec(fs.readFileSync("/proc/meminfo", "utf8"))[1]) * 1024;
  return {
    at: new Date().toISOString(),
    load1: +l1.toFixed(2), load5: +l5.toFixed(2), load15: +l15.toFixed(2),
    memUsedPct: +(100 * (1 - avail / os.totalmem())).toFixed(1),
    buildProcs: buildProcs(),
    top: topCpu(),
  };
}
async function gate(label) {
  const start = Date.now();
  let s = loadSnapshot();
  let waited = 0;
  while (GATE && (s.load1 >= 3 || s.buildProcs.length > 0)) {
    if (Date.now() - start > MAX_WAIT_MIN * 60000) {
      log(`${label}: machine not quiet after ${MAX_WAIT_MIN} min (load ${s.load1}, ${s.buildProcs.join(" ")}) — running, FLAGGED`);
      return { status: "timeout-noisy", waitedMin: +(waited / 60000).toFixed(1), ...s };
    }
    log(`${label}: waiting (load ${s.load1}, build procs: ${s.buildProcs.join(" ") || "none"})`);
    await sleep(120000);
    waited = Date.now() - start;
    s = loadSnapshot();
  }
  return { status: GATE ? "quiet" : "ungated", waitedMin: +(waited / 60000).toFixed(1), ...s };
}

// Measurements must not overlap a build: before every process run, wait until no build process is
// running (same poll/limit as the gate); after it, record whether one appeared during the run.
async function noBuilds(label) {
  const start = Date.now();
  let b = buildProcs();
  while (GATE && b.length && Date.now() - start < MAX_WAIT_MIN * 60000) {
    log(`${label}: build running (${b.join(" ")}), waiting before the next run`);
    await sleep(120000);
    b = buildProcs();
  }
  return b;
}
const overlap = (pre) => {
  const post = buildProcs();
  return { buildProcsAtStart: pre, buildProcsAtEnd: post, overlappedBuild: pre.length > 0 || post.length > 0 };
};

// ── process helpers ────────────────────────────────────────────────────────────────────────────
function pss(pid) {
  try {
    const m = /^Pss:\s+(\d+) kB/m.exec(fs.readFileSync(`/proc/${pid}/smaps_rollup`, "utf8"));
    return m ? Number(m[1]) : null;
  } catch {
    return null;
  }
}
function descendants(root) {
  const kids = new Map();
  for (const pid of fs.readdirSync("/proc").filter((d) => /^\d+$/.test(d))) {
    try {
      const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
      const ppid = Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[1]);
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(Number(pid));
    } catch {}
  }
  const out = [];
  const walk = (p) => (kids.get(p) ?? []).forEach((c) => { out.push(c); walk(c); });
  walk(root);
  return out;
}
function cmdline(pid) {
  try {
    return fs.readFileSync(`/proc/${pid}/cmdline`, "utf8").split("\0").join(" ");
  } catch {
    return "";
  }
}
// Every process of a Chromium tree with its PSS; role from --type=.
function chromeTree(root) {
  return [root, ...descendants(root)].map((pid) => {
    const c = cmdline(pid);
    const role = /--type=([\w-]+)/.exec(c)?.[1] ?? "browser";
    return { pid, role: /--extension-process/.test(c) ? "extension" : role, pss: pss(pid) };
  });
}

// Spawns a process (own process group), calls onLine(line, tMs) per output line; resolves on `until`.
function launch(cmd, argv, env, onLine) {
  const t0 = performance.now();
  const child = spawn(cmd, argv, { env: { ...process.env, ...env }, detached: true, stdio: ["ignore", "pipe", "pipe"] });
  let buf = "";
  const feed = (d) => {
    const t = performance.now() - t0;
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i);
      buf = buf.slice(i + 1);
      onLine(line, t);
    }
  };
  child.stdout.on("data", feed);
  child.stderr.on("data", feed);
  const exited = new Promise((r) => child.on("exit", r));
  return {
    child, exited,
    stop: async () => {
      try { process.kill(-child.pid, "SIGTERM"); } catch {}
      const t = setTimeout(() => { try { process.kill(-child.pid, "SIGKILL"); } catch {} }, 5000);
      await exited;
      clearTimeout(t);
    },
  };
}

// ── native ─────────────────────────────────────────────────────────────────────────────────────
const NATIVE_ENV = {
  QT_QPA_PLATFORM: "offscreen", QT_QUICK_BACKEND: "rhi", QSG_RHI_BACKEND: "null",
  QT_FORCE_STDERR_LOGGING: "1", QSG_INFO: "1",
};
// The window's frameSwapped is the native "presented frame" (there is no requestAnimationFrame in
// QML). update() forces a frame even when the mutation changed nothing visible (e.g. a swap of two
// virtualized rows outside the viewport), so every operation gets a frame, as rAF does on the web.
const NATIVE_PRESENTED = `    property var __frameWaiters: []
    Connections {
        target: __self.Window.window
        function onFrameSwapped() { var q = __self.__frameWaiters; if (q.length === 0) return; __self.__frameWaiters = []; for (var i = 0; i < q.length; i++) q[i](); }
    }
    function presented(cb) { __frameWaiters.push(cb); if (__self.Window.window) __self.Window.window.update(); }`;

function genNative(entry, name) {
  const base = path.join(WORK, `gen-${name}`);
  fs.mkdirSync(base, { recursive: true });
  execFileSync(process.execPath, ["--import", "tsx", "scripts/gen.mjs", entry, path.join(base, "App.generated.qml")],
    { cwd: ROOT, stdio: ["ignore", "ignore", "inherit"] });
  // gen.mjs OVERWRITES App.generated.css: the bench CSS is appended after it.
  fs.appendFileSync(path.join(base, "App.generated.css"), fs.readFileSync(path.join(ROOT, "examples/bench-rows.css")));
  return base;
}
function nativeVariant(base, comp, { scroll, key }) {
  const d = path.join(WORK, `${path.basename(base)}-${scroll}-${key ? "key" : "nokey"}`);
  if (fs.existsSync(d)) return d;
  fs.cpSync(base, d, { recursive: true });
  const cssFile = path.join(d, "App.generated.css");
  fs.writeFileSync(cssFile, fs.readFileSync(cssFile, "utf8").replace(/overflow-y: auto/, `overflow-y: ${scroll}`));
  const qmlFile = path.join(d, comp);
  let q = fs.readFileSync(qmlFile, "utf8");
  if (key) {
    // Identical to what the transpiler emits for <For each={rows()} key="id">.
    const before = q;
    q = q.replace(/^(\s*)model: rows$/m, `$1model: rows\n$1key: "id"`);
    if (q === before) throw new Error("key insertion point not found");
  }
  if (/^\s*function presented\(cb\) \{.*\}$/m.test(q))
    q = q.replace(/^\s*function presented\(cb\) \{.*\}$/m, NATIVE_PRESENTED);
  fs.writeFileSync(qmlFile, q);
  return d;
}
const loaderArgs = (d) => ["--qml", path.join(d, "App.generated.qml"), "--css", path.join(d, "App.generated.css"), "--width", "800", "--height", "900"];

// ── web ────────────────────────────────────────────────────────────────────────────────────────
function buildWeb(entry, name) {
  const out = path.join(WORK, `web-${name}`);
  execFileSync("npx", ["vite", "build", "--outDir", out, "--emptyOutDir", "--logLevel", "warn"],
    { cwd: ROOT, env: { ...process.env, SQ_ENTRY: entry }, stdio: ["ignore", "ignore", "inherit"] });
  return out;
}
function serve(dirs) {
  const types = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css" };
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, "http://x");
    const [, app, ...rest] = u.pathname.split("/");
    const root = dirs[app];
    let p = rest.join("/") || "index.html";
    const f = root && path.join(root, p);
    if (!f || !f.startsWith(root) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { "content-type": types[path.extname(f)] ?? "application/octet-stream" });
    fs.createReadStream(f).pipe(res);
  });
  return new Promise((r) => server.listen(0, "127.0.0.1", () => r(server)));
}
function chromeFlags(profile) {
  return [
    "--headless=new", "--enable-logging=stderr", "--v=0", `--user-data-dir=${profile}`,
    "--window-size=800,900", "--no-first-run", "--no-default-browser-check", "--disable-extensions",
    "--disable-background-networking", "--disable-component-update", "--disable-sync",
    "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
  ];
}
// The built index.html references /assets/…; rewrite to the per-app prefix the server uses.
function prefixAssets(dir, app) {
  const f = path.join(dir, "index.html");
  fs.writeFileSync(f, fs.readFileSync(f, "utf8").replace(/"\/assets\//g, `"/${app}/assets/`));
}

// ── one process run ────────────────────────────────────────────────────────────────────────────
const BENCH_RE = /BENCH (\w+) ([\d.]+) \+tick ([\d.]+) \+frame ([\d.]+) round (\d+)/;

async function runRows(target, cfg, rep) {
  const samples = [];
  let done = false;
  let resolveDone;
  const finished = new Promise((r) => (resolveDone = r));
  const info = [];
  const onLine = (line) => {
    const m = BENCH_RE.exec(line);
    if (m) {
      samples.push({ op: m[1], sync: +m[2], tick: +m[3], frame: +m[4], round: +m[5], warmup: +m[5] === 0 });
    } else if (/BENCH done/.test(line)) {
      done = true;
      resolveDone();
    } else if (target === "native" && /qt\.scenegraph\.general|render loop|Using sg/i.test(line)) {
      info.push(line.trim());
    }
  };
  const load = loadSnapshot();
  let p;
  if (target === "native") {
    p = launch(LOADER, loaderArgs(cfg.dir), { ...NATIVE_ENV, BENCH_N: String(cfg.n), BENCH_ROUNDS: String(ROUNDS) }, onLine);
  } else {
    const profile = fs.mkdtempSync(path.join(WORK, "chrome-"));
    const url = `${cfg.url}?n=${cfg.n}&rounds=${ROUNDS}&overflow=${cfg.scroll}`;
    p = launch(CHROMIUM, [...chromeFlags(profile), url], {}, onLine);
  }
  const timer = new Promise((r) => setTimeout(r, RUN_TIMEOUT_MS).unref());
  await Promise.race([finished, timer, p.exited]);
  await p.stop();
  return { target, scroll: cfg.scroll, n: cfg.n, key: cfg.key, rep, complete: done, load1AtStart: load.load1, rendererInfo: [...new Set(info)].slice(0, 8), samples };
}

async function runMem(target, cfg, rep) {
  const r = { target, scroll: cfg.scroll, rep, rows: 2000, startupMs: null, base: null, after: null, complete: false };
  let p;
  let resolveDone;
  const finished = new Promise((x) => (resolveDone = x));
  let rootPid = null;
  const snap = () => (target === "native" ? [{ pid: rootPid, role: "loader", pss: pss(rootPid) }] : chromeTree(rootPid));
  const onLine = (line, t) => {
    if (/MEM mounted/.test(line) && r.startupMs === null) r.startupMs = +t.toFixed(1);
    else if (/MEM base/.test(line) && !r.base) r.base = snap();
    else if (/MEM rows/.test(line) && !r.after) {
      setTimeout(() => { r.after = snap(); r.complete = true; resolveDone(); }, MEM_SETTLE_MS);
    }
  };
  r.load1AtStart = loadSnapshot().load1;
  if (target === "native") {
    p = launch(LOADER, loaderArgs(cfg.dir), NATIVE_ENV, onLine);
  } else {
    const profile = fs.mkdtempSync(path.join(WORK, "chrome-"));
    p = launch(CHROMIUM, [...chromeFlags(profile), `${cfg.url}?overflow=${cfg.scroll}`], {}, onLine);
  }
  rootPid = p.child.pid;
  await Promise.race([finished, new Promise((x) => setTimeout(x, 120000).unref()), p.exited]);
  await p.stop();
  if (r.complete) {
    const byPid = new Map(r.base.map((b) => [b.pid, b.pss]));
    r.processes = r.after.map((a) => ({ ...a, deltaKB: byPid.has(a.pid) && a.pss != null ? a.pss - byPid.get(a.pid) : null }));
    const main = target === "native" ? r.processes[0]
      : r.processes.filter((x) => x.role === "renderer" && x.deltaKB != null).sort((a, b) => b.deltaKB - a.deltaKB)[0];
    r.mainDeltaKB = main?.deltaKB ?? null;
    r.mainPssKB = main?.pss ?? null;
    r.treeDeltaKB = r.processes.reduce((s, x) => s + (x.deltaKB ?? 0), 0);
    r.perRowKB = r.mainDeltaKB != null ? +(r.mainDeltaKB / r.rows).toFixed(2) : null;
    r.treePerRowKB = +(r.treeDeltaKB / r.rows).toFixed(2);
  }
  delete r.base;
  delete r.after;
  return r;
}

// ── main ───────────────────────────────────────────────────────────────────────────────────────
async function main() {
  if (TARGETS.includes("native") && !fs.existsSync(LOADER)) throw new Error(`no loader at ${LOADER}`);
  const env = environment();
  if (TARGETS.includes("native") && !env.build.upToDate) throw new Error(`${BUILD} is stale — rebuild first`);
  log("environment", JSON.stringify({ lib: env.build.libSha256, buildtype: env.build.buildtype, qt: env.qt, chromium: env.chromium }));

  const nat = {};
  const web = {};
  let server;
  if (TARGETS.includes("native")) {
    nat.rows = genNative("examples/bench-rows.qml.tsx", "rows");
    nat.mem = genNative("examples/bench-mem.qml.tsx", "mem");
  }
  if (TARGETS.includes("web")) {
    const rows = buildWeb("examples/bench-rows.web.tsx", "rows");
    const mem = buildWeb("examples/bench-mem.web.tsx", "mem");
    prefixAssets(rows, "rows");
    prefixAssets(mem, "mem");
    server = await serve({ rows, mem });
    const port = server.address().port;
    web.rows = `http://127.0.0.1:${port}/rows/`;
    web.mem = `http://127.0.0.1:${port}/mem/`;
  }

  const result = { environment: env, batches: [], runs: [], mem: [] };
  const stamp = env.date.slice(0, 10);
  const name = `${stamp}-${env.host}${args.tag ? "-" + args.tag : ""}`;
  fs.mkdirSync(OUT, { recursive: true });
  const save = () => {
    fs.writeFileSync(path.join(OUT, `${name}.json`), JSON.stringify(result, null, 1) + "\n");
    writeCsv(path.join(OUT, `${name}.csv`), result);
  };

  const configs = [];
  for (const n of SIZES)
    for (const scroll of ["auto", "visible"]) {
      if (TARGETS.includes("native")) {
        configs.push({ target: "native", n, scroll, key: false });
        configs.push({ target: "native", n, scroll, key: true });
      }
      if (TARGETS.includes("web")) configs.push({ target: "web", n, scroll, key: false });
    }

  // --only native:auto:1000:key,web:auto:1000 — re-measure selected configurations (e.g. runs that
  // overlapped a build); report.mjs merges files and drops runs flagged overlappedBuild.
  const only = args.only?.split(",").map((x) => x.split(":"));
  const selected = only
    ? configs.filter((c) => only.some(([t, s, n, k]) => t === c.target && s === c.scroll && Number(n) === c.n && (k === "key") === c.key))
    : configs;
  for (const c of selected) {
    const label = `${c.target} ${c.scroll} N=${c.n}${c.key ? " key" : ""}`;
    const batch = { label, kind: "rows", gate: await gate(label) };
    const cfg = { ...c, dir: c.target === "native" ? nativeVariant(nat.rows, "BenchRows.qml", c) : null, url: web.rows };
    for (let rep = 0; rep < REPS; rep++) {
      log(`${label} rep ${rep + 1}/${REPS}`);
      const pre = await noBuilds(label);
      const r = await runRows(c.target, cfg, rep);
      Object.assign(r, overlap(pre));
      if (!r.complete) log(`${label}: INCOMPLETE run (${r.samples.length} samples)`);
      result.runs.push(r);
    }
    batch.after = loadSnapshot();
    result.batches.push(batch);
    save();
  }

  if (MEM_REPS > 0) {
    for (const scroll of ["auto", "visible"])
      for (const target of TARGETS) {
        const label = `mem ${target} ${scroll}`;
        const batch = { label, kind: "mem", gate: await gate(label) };
        const cfg = { scroll, dir: target === "native" ? nativeVariant(nat.mem, "BenchMem.qml", { scroll, key: false }) : null, url: web.mem };
        for (let rep = 0; rep < MEM_REPS; rep++) {
          const pre = await noBuilds(label);
          const r = await runMem(target, cfg, rep);
          Object.assign(r, overlap(pre));
          if (!r.complete) log(`${label}: INCOMPLETE mem run`);
          result.mem.push(r);
        }
        batch.after = loadSnapshot();
        result.batches.push(batch);
        save();
      }
  }
  server?.close();
  save();
  fs.rmSync(WORK, { recursive: true, force: true });
  log(`wrote ${path.relative(ROOT, path.join(OUT, name))}.{json,csv,mem.csv}`);
}

function writeCsv(file, result) {
  const rows = ["target,scroll,n,key,rep,round,warmup,op,sync_ms,tick_ms,frame_ms,load1_at_start"];
  for (const r of result.runs)
    for (const s of r.samples)
      rows.push([r.target, r.scroll, r.n, r.key, r.rep, s.round, s.warmup, s.op, s.sync, s.tick, s.frame, r.load1AtStart].join(","));
  fs.writeFileSync(file, rows.join("\n") + "\n");
  const mem = ["target,scroll,rep,rows,startup_ms,main_pss_kb,main_delta_kb,per_row_kb,tree_delta_kb,tree_per_row_kb,load1_at_start"];
  for (const m of result.mem)
    mem.push([m.target, m.scroll, m.rep, m.rows, m.startupMs, m.mainPssKB, m.mainDeltaKB, m.perRowKB, m.treeDeltaKB, m.treePerRowKB, m.load1AtStart].join(","));
  fs.writeFileSync(file.replace(/\.csv$/, ".mem.csv"), mem.join("\n") + "\n");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
