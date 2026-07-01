// Native hot-reload dev loop driven by the transpiler.
//   run via:  node --import tsx scripts/dev-native.mjs
//
//   src/mainqml.tsx  --(generate)-->  qml/solidqml/App.generated.qml + <Component>.qml + .css
//                                          --(loader --watch)--> reloads the live window
//
// Requires the loader built once: `meson setup build && ninja -C build`.
import { spawn } from "node:child_process";
import { writeFile, readFile, mkdir, readdir, rm } from "node:fs/promises";
import { watch } from "node:fs";
import { generate } from "../transpiler/src/index.ts";

const ENTRY = "src/mainqml.tsx";
const OUT = "qml/solidqml";
const QML = `${OUT}/App.generated.qml`;
const CSS = `${OUT}/App.generated.css`;
const MODULES = `${OUT}/modules`;
const LOADER = "build/solid-qml-loader";

// Component types the transpiler emits, written flat next to the entry (QML resolves sibling .qml by filename).
let lastComponents = new Set();
let lastModules = new Set();

async function gen() {
  try {
    const app = await generate(await readFile(ENTRY, "utf8"), ENTRY);
    await mkdir(OUT, { recursive: true });
    // Remove component .qml files from the previous generation that are no longer emitted.
    const now = new Set(Object.keys(app.components));
    for (const name of lastComponents) if (!now.has(name)) await rm(`${OUT}/${name}.qml`, { force: true });
    lastComponents = now;
    for (const [name, qml] of Object.entries(app.components)) await writeFile(`${OUT}/${name}.qml`, qml);
    const nowModules = new Set(Object.keys(app.modules));
    await mkdir(MODULES, { recursive: true });
    const existingModules = await readdir(MODULES).catch(() => []);
    for (const file of existingModules) {
      const name = `modules/${file}`;
      if (file.endsWith(".mjs") && !nowModules.has(name)) await rm(`${OUT}/${name}`, { force: true });
    }
    for (const name of lastModules) if (!nowModules.has(name)) await rm(`${OUT}/${name}`, { force: true });
    lastModules = nowModules;
    for (const [name, source] of Object.entries(app.modules)) await writeFile(`${OUT}/${name}`, source);
    await writeFile(CSS, app.css);
    await writeFile(QML, app.entry); // write the entry LAST so the loader reloads a complete tree
    console.log(`[dev] generated ${QML} + ${now.size} components + ${nowModules.size} modules + css (${app.css.length}b)`);
  } catch (err) {
    console.error("[dev] generate failed:", err.message);
  }
}

await gen();

const loader = spawn(LOADER, ["--qml", QML, "--css", CSS, "--watch"], {
  stdio: "inherit",
  env: { ...process.env, QML_DISABLE_DISK_CACHE: "1" },
});
loader.on("error", (err) => {
  console.error(`[dev] cannot start ${LOADER} (build it: ninja -C build):`, err.message);
  process.exit(1);
});
loader.on("exit", (code) => process.exit(code ?? 0));

let timer;
const onChange = (dir) => (_event, file) => {
  if (!file || !/\.[jt]sx?$/.test(file)) return;
  clearTimeout(timer);
  timer = setTimeout(() => { console.log(`[dev] ${dir}/${file} changed → regenerate`); gen(); }, 100);
};
watch("src", { recursive: true }, onChange("src"));
watch("examples", { recursive: true }, onChange("examples"));
console.log("[dev] watching src + examples → QML; loader --watch reloads the native window.");
