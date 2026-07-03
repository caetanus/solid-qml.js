// One-shot QML generation via the transpiler: TSX entry → App.generated.qml + <Component>.qml +
// modules + App.generated.css. Used by the native dev loop (scripts/dev-native.mjs) and the Vite
// plugin's native target. Run with tsx: `node --import tsx scripts/gen.mjs [entry] [qmlOutput]`.
import { generate } from "../transpiler/src/index.ts";
import { writeFile, readFile, mkdir, cp } from "node:fs/promises";
import path from "node:path";

const entry = process.argv[2] ?? "src/mainqml.tsx";
const out = process.argv[3] ?? "qml/solidqml/App.generated.qml";
const outDir = path.dirname(out);

const app = await generate(await readFile(entry, "utf8"), entry);
await mkdir(path.join(outDir, "modules"), { recursive: true });
for (const [name, qml] of Object.entries(app.components)) await writeFile(path.join(outDir, `${name}.qml`), qml);
for (const [name, source] of Object.entries(app.modules)) await writeFile(path.join(outDir, name), source);
await writeFile(out.replace(/\.qml$/, ".css"), app.css);
await writeFile(out, app.entry);
// Static assets referenced as "assets/…" resolve relative to the generated QML — mirror them.
await cp("assets", path.join(outDir, "assets"), { recursive: true }).catch(() => {});
console.log(`[gen] ${Object.keys(app.components).length} components + ${Object.keys(app.modules).length} modules → ${out}`);
