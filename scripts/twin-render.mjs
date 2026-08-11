// Twin-render harness (M-AOT-0 verification): renders the SAME TSX through both back-ends and
// asserts the frames are pixel-identical.
//   (a) interpreted: transpiler → QML, loaded by build/solid-qml-loader
//   (b) AOT:         transpiler cpp back-end → C++, compiled as build/aot-counter
// The QML render is the reference; any structural diff is an AOT codegen bug. Run:
//   node --import tsx scripts/twin-render.mjs [entry.tsx] [maxDiffPct]
// maxDiffPct (default 0 = pixel-exact) tolerates non-structural variance — @keyframes animation
// phase at grab time and sub-pixel text antialiasing on centered labels — for views that have it.
// Requires: build/ configured (meson) with the loader + aot features, offscreen Qt, python3+PIL.
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { generate } from "../transpiler/src/index.ts";
import { generateCpp } from "../transpiler/src/emit/cpp/index.ts";

const run = promisify(execFile);
const entry = process.argv[2] ?? "examples/aot/counter.tsx";
const maxDiffPct = Number(process.argv[3] ?? 0);
const root = process.cwd();
const env = { ...process.env, QT_QPA_PLATFORM: "offscreen", QT_FORCE_STDERR_LOGGING: "1", SQ_GRAB_MS: "1400" };

const work = await mkdtemp(path.join(tmpdir(), "twin-"));
const src = await readFile(entry, "utf8");

// (a) QML back-end → reference frame.
const app = await generate(src, entry);
const qmlPath = path.join(work, "App.generated.qml");
const cssPath = path.join(work, "App.generated.css");
await writeFile(qmlPath, app.entry);
await writeFile(cssPath, app.css);
for (const [name, qml] of Object.entries(app.components)) await writeFile(path.join(work, `${name}.qml`), qml);
const refPng = path.join(work, "ref.png");
await run("build/solid-qml-loader", ["--qml", qmlPath, "--css", "src/solid-qml/base.css", "--css", cssPath, "--grab", refPng], { env, cwd: root });

// (b) AOT C++ back-end → generated binary frame. (Regenerate + rebuild so the check reflects HEAD.)
await run("node", ["--import", "tsx", "scripts/gen-cpp.mjs", entry, "examples/aot/generated"], { cwd: root });
await run("ninja", ["-C", "build", "aot-counter"], { cwd: root });
const aotPng = path.join(work, "aot.png");
await run("build/aot-counter", ["--css", "src/solid-qml/base.css", "--css", cssPath, "--grab", aotPng], { env, cwd: root });

// Pixel compare (PIL). Exit non-zero on any difference.
const py = `
from PIL import Image, ImageChops
import numpy as np, sys
tol=float(sys.argv[3])
a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB')
if a.size!=b.size: print('SIZE MISMATCH', a.size, b.size); sys.exit(1)
d=np.asarray(ImageChops.difference(a,b))
total=a.size[0]*a.size[1]; nz=int((d.sum(2)>0).sum()); pct=100.0*nz/total
print(f'max diff {d.max()}, {nz}/{total} px differ ({pct:.3f}%), match {100-pct:.3f}%')
sys.exit(0 if pct<=tol else 1)
`;
try {
  const { stdout } = await run("python3", ["-c", py, refPng, aotPng, String(maxDiffPct)], { cwd: root });
  const verdict = maxDiffPct === 0 ? "PIXEL-IDENTICAL ✓" : `within ${maxDiffPct}% tolerance ✓`;
  process.stdout.write(`twin-render ${entry}: ${stdout.trim()} → ${verdict}\n`);
} catch (e) {
  process.stdout.write(`twin-render ${entry}: ${(e.stdout ?? "").trim()} → DIFF ✗ (ref=${refPng} aot=${aotPng})\n`);
  process.exit(1);
}
