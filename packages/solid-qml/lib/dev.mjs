// `solid-qml dev` — the native dev loop: TSX → QML into .solid-qml/dev, open it in the prebuilt
// runtime's loader with --watch, and regenerate on every source/CSS/asset edit (the loader then
// hot-reloads the live window). The browser preview is plain `vite` (npm run dev).
import { spawn } from "node:child_process";
import { existsSync, watch } from "node:fs";
import { mkdir } from "node:fs/promises";
import path from "node:path";
import { generateApp } from "./generate.mjs";
import { loadProject } from "./project.mjs";
import { findRuntime } from "./runtime.mjs";

const WATCHED = /\.(tsx|ts|jsx|js|mjs|css|qml|json|svg|png|jpe?g|gif|webp|ttf|otf|woff2?)$/i;

export async function dev(opts) {
  const project = await loadProject(opts.cwd);
  const runtime = findRuntime(project.dir);
  const outDir = path.join(project.workDir, "dev");
  await mkdir(outDir, { recursive: true });

  const state = {};
  const regenerate = async () => {
    try {
      const r = await generateApp(project, outDir, state);
      console.log(`[solid-qml] generated ${path.relative(project.dir, r.qml)} + ${r.components} components + ${r.modules} modules + css (${r.cssBytes}b)`);
      return true;
    } catch (err) {
      console.error(`[solid-qml] generate failed: ${err.message}`);
      return false;
    }
  };
  if (!(await regenerate()) && opts.grab) process.exit(1);

  const args = ["--app-name", project.appId, "--qml", path.join(outDir, "App.generated.qml")];
  if (opts.grab) args.push("--grab", path.resolve(opts.grab));
  else args.push("--watch");
  args.push(...opts.loaderArgs);

  const loader = spawn(runtime.launcher, args, {
    stdio: "inherit",
    env: { QML_DISABLE_DISK_CACHE: "1", QT_FORCE_STDERR_LOGGING: "1", ...process.env },
  });
  loader.on("error", (err) => {
    console.error(`[solid-qml] cannot start ${runtime.launcher}: ${err.message} (try: npx solid-qml doctor)`);
    process.exit(1);
  });
  loader.on("exit", (code, signal) => process.exit(code ?? (signal ? 1 : 0)));
  if (opts.grab) return; // one frame, then the loader exits

  let timer;
  const onChange = (dir) => (_event, file) => {
    if (!file || !WATCHED.test(file) || file.includes("node_modules")) return;
    clearTimeout(timer);
    timer = setTimeout(() => {
      console.log(`[solid-qml] ${path.join(dir, file)} changed → regenerate`);
      regenerate();
    }, 100);
  };
  // The source tree (entry dir and src/), static files, never the generated .solid-qml/.
  const dirs = new Set([path.dirname(project.entry), path.join(project.dir, "src"), path.join(project.dir, "public"), path.join(project.dir, "assets")]);
  for (const dir of dirs) {
    if (!existsSync(dir) || dir === project.dir) continue;
    watch(dir, { recursive: true }, onChange(path.relative(project.dir, dir)));
  }
  console.log(`[solid-qml] watching ${[...dirs].filter(existsSync).map((d) => path.relative(project.dir, d) || ".").join(", ")} — edits hot-reload the native window`);
  const stop = () => loader.kill("SIGTERM");
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}
