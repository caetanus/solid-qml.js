// M-AOT-2 benchmark: the SAME TSX rendered by the interpreted loader (parses/JITs generated QML) vs
// the AOT binary (the scene is C++ — no QML parse), measured for stripped binary size, cold-start
// wall time (grab delay minimized so the delta is init+parse+first-render) and peak RSS (VmHWM).
//   node --import tsx scripts/bench-aot.mjs [entry.tsx]
// Requires: build/ configured with the loader + aot features, offscreen Qt, python3, strip.
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { mkdtemp, readFile, writeFile, copyFile, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { generate } from "../transpiler/src/index.ts";

const run = promisify(execFile);
const entry = process.argv[2] ?? "examples/aot/counter.tsx";
const root = process.cwd();
const work = await mkdtemp(path.join(tmpdir(), "bench-"));

// Generate the QML (for the loader) and the C++ (for the AOT binary), then build both.
const app = await generate(await readFile(entry, "utf8"), entry);
const qml = path.join(work, "App.generated.qml"), css = path.join(work, "App.generated.css");
await writeFile(qml, app.entry);
await writeFile(css, app.css);
for (const [n, q] of Object.entries(app.components)) await writeFile(path.join(work, `${n}.qml`), q);
await run("node", ["--import", "tsx", "scripts/gen-cpp.mjs", entry, "examples/aot/generated"], { cwd: root });
await run("ninja", ["-C", "build", "aot-counter", "solid-qml-loader"], { cwd: root });

const aot = path.join(work, "aot"), loader = path.join(work, "loader");
await copyFile("build/aot-counter", aot);
await copyFile("build/solid-qml-loader", loader);
await run("strip", [aot, loader]).catch(() => {});
const kb = async (p) => Math.round((await stat(p)).size / 1024);

const py = `
import subprocess, time, os, statistics, sys
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1", SQ_GRAB_MS="60")
def hwm(pid):
    try:
        for l in open(f"/proc/{pid}/status"):
            if l.startswith("VmHWM"): return int(l.split()[1])
    except: pass
    return 0
def run(cmd, n=7):
    w, r = [], []
    for _ in range(n):
        t0 = time.monotonic(); p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); hi = 0
        while p.poll() is None: hi = max(hi, hwm(p.pid)); time.sleep(0.002)
        w.append((time.monotonic()-t0)*1000); r.append(hi)
    return statistics.median(w), statistics.median(r)
aot = sys.argv[1].split("|"); ld = sys.argv[2].split("|")
aw, ar = run(aot); lw, lr = run(ld)
print(f"{aw:.0f} {ar} {lw:.0f} {lr}")
`;
// Time the ORIGINAL build/ binaries (their RPATH finds libsolidqml.so); the /tmp copies are only
// for the stripped-size figure.
const aotCmd = ["build/aot-counter", "--css", "src/solid-qml/base.css", "--css", css, "--grab", path.join(work, "a.png")].join("|");
const ldCmd = ["build/solid-qml-loader", "--qml", qml, "--css", "src/solid-qml/base.css", "--css", css, "--grab", path.join(work, "l.png")].join("|");
const { stdout } = await run("python3", ["-c", py, aotCmd, ldCmd], { cwd: root });
const [aw, ar, lw, lr] = stdout.trim().split(/\s+/).map(Number);

const mib = (k) => (k / 1024).toFixed(1);
process.stdout.write(
  `\nM-AOT-2 bench — ${entry}\n` +
  `                      binary   cold-start   peak RSS\n` +
  `  AOT (C++ scene)     ${String(await kb(aot)).padStart(4)}K     ${String(aw).padStart(4)} ms     ${mib(ar).padStart(5)} MiB\n` +
  `  loader (QML interp) ${String(await kb(loader)).padStart(4)}K     ${String(lw).padStart(4)} ms     ${mib(lr).padStart(5)} MiB\n` +
  `  AOT advantage       ${String(await kb(loader) - await kb(aot)).padStart(4)}K     ${String(lw - aw).padStart(4)} ms     ${mib(lr - ar).padStart(5)} MiB` +
  `  (${(((lw - aw) / lw) * 100).toFixed(0)}% faster start)\n` +
  `  Both link the shared libsolidqml.so (CSS engine + widgets + shims); the AOT win is the removed\n` +
  `  QML parse/compile at startup. It grows with scene size; a full-gallery + Electron comparison is\n` +
  `  the real M-AOT-2 deliverable (pending broader construct coverage).\n`,
);
