// TSX → QML for one app: the transpiler's multi-file output (entry + one .qml per component +
// mirrored npm modules + the CSS sidecar) written into an output directory, plus the app's
// static files (Vite's `public/`, and a legacy `assets/`) mirrored beside it so "assets/x.png"
// resolves relative to the generated QML exactly as it does relative to the web page.
import { cp, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";
import { PACKAGE_DIR } from "./project.mjs";

let transpiler;
async function loadTranspiler() {
  // dist/ is the published bundle; inside the solid-qml repo (workspace link, before
  // `npm run sdk:build`) fall back to a clear error rather than a cryptic import failure.
  const bundled = path.join(PACKAGE_DIR, "dist/transpiler.mjs");
  if (!existsSync(bundled)) throw new Error(`${bundled} is missing — run \`npm run sdk:build\` in the solid-qml repo`);
  transpiler ??= await import(bundled);
  return transpiler;
}

export const ENTRY_QML = "App.generated.qml";

/**
 * @param {ReturnType<typeof import("./project.mjs").loadProject> extends Promise<infer P> ? P : never} project
 * @param {string} outDir
 * @param {{ components?: Set<string>, modules?: Set<string> }} [state] previous generation (dev loop) — stale files are removed
 */
export async function generateApp(project, outDir, state = {}) {
  const { generate } = await loadTranspiler();
  const app = await generate(await readFile(project.entry, "utf8"), project.entry);
  await mkdir(path.join(outDir, "modules"), { recursive: true });

  const components = new Set(Object.keys(app.components));
  for (const name of state.components ?? []) if (!components.has(name)) await rm(path.join(outDir, `${name}.qml`), { force: true });
  for (const [name, qml] of Object.entries(app.components)) await writeFile(path.join(outDir, `${name}.qml`), qml);

  const modules = new Set(Object.keys(app.modules));
  for (const name of state.modules ?? []) if (!modules.has(name)) await rm(path.join(outDir, name), { force: true });
  for (const [name, source] of Object.entries(app.modules)) {
    await mkdir(path.dirname(path.join(outDir, name)), { recursive: true });
    await writeFile(path.join(outDir, name), source);
  }

  for (const dir of ["public", "assets"]) {
    const src = path.join(project.dir, dir);
    if (!existsSync(src)) continue;
    const dest = dir === "public" ? outDir : path.join(outDir, "assets");
    for (const entry of await readdir(src)) {
      await cp(path.join(src, entry), path.join(dest, entry), { recursive: true, force: true });
    }
  }

  const qml = path.join(outDir, ENTRY_QML);
  await writeFile(qml.replace(/\.qml$/, ".css"), app.css);
  await writeFile(qml, app.entry); // LAST: the loader's --watch reloads on this file
  state.components = components;
  state.modules = modules;
  return { qml, components: components.size, modules: modules.size, cssBytes: app.css.length };
}
