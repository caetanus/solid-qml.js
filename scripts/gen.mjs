// One-shot QML generation via the transpiler: TSX entry → App.generated.qml + <Component>.qml +
// modules + App.generated.css. Used by the native dev loop (scripts/dev-native.mjs) and the Vite
// plugin's native target. Run with tsx: `node --import tsx scripts/gen.mjs [entry] [qmlOutput]`.
import { generate } from "../transpiler/src/index.ts";
import { writeFile, readFile, mkdir, cp } from "node:fs/promises";
import path from "node:path";

const entry = process.argv[2] ?? "src/mainqml.tsx";
const out = process.argv[3] ?? "qml/solidqml/App.generated.qml";
const outDir = path.dirname(out);

let app;
try {
  app = await generate(await readFile(entry, "utf8"), entry);
} catch (e) {
  // A transpile error is a message for the app author, not a crash: print it without the stack
  // (SQ_GEN_STACK=1 keeps the stack, for debugging the transpiler itself).
  console.error(`[gen] ${entry}: ${process.env.SQ_GEN_STACK ? e.stack : e.message}`);
  process.exit(1);
}
await mkdir(path.join(outDir, "modules"), { recursive: true });
for (const [name, qml] of Object.entries(app.components)) await writeFile(path.join(outDir, `${name}.qml`), qml);
for (const [name, source] of Object.entries(app.modules)) await writeFile(path.join(outDir, name), source);
await writeFile(out.replace(/\.qml$/, ".css"), app.css);
await writeFile(out, app.entry);
// Static assets referenced as "assets/…" resolve relative to the generated QML — mirror them.
// Resolved against process.cwd() (an "assets/" dir alongside wherever you invoke this from), not
// the entry file's own directory — run this with cwd = your app's root if it has one (see
// docs/getting-started.md, "Your first app outside the checkout"). A missing dir is silently
// skipped (apps with no static assets are the common case, not an error).
await cp("assets", path.join(outDir, "assets"), { recursive: true }).catch(() => {});
console.log(`[gen] ${Object.keys(app.components).length} components + ${Object.keys(app.modules).length} modules → ${out}`);
