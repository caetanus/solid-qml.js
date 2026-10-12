// Locating the prebuilt native runtime: the `@solid-qml/runtime-<platform>-<arch>` npm package
// (installed as an optionalDependency of `solid-qml`, esbuild-style: its os/cpu fields make npm
// install only the host's one). `build --target X` on another host needs that target's package
// installed explicitly (`npm i -D @solid-qml/runtime-linux-x64 --force`).
import { existsSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { CliError, PACKAGE_DIR } from "./project.mjs";

export const TARGETS = { linux: "linux-x64" };

export function hostKey() {
  return `${process.platform}-${process.arch}`;
}

function readManifest(pkgDir) {
  const file = path.join(pkgDir, "runtime.json");
  if (!existsSync(file)) return null;
  const manifest = JSON.parse(readFileSync(file, "utf8"));
  return {
    dir: pkgDir,
    manifest,
    root: path.join(pkgDir, manifest.root ?? "runtime"),
    launcher: path.join(pkgDir, manifest.launcher),
    loader: path.join(pkgDir, manifest.loader),
  };
}

/**
 * @param {string} projectDir
 * @param {string} key e.g. "linux-x64"
 * @returns {{dir:string, manifest:any, root:string, launcher:string, loader:string}}
 */
export function findRuntime(projectDir, key = hostKey()) {
  // Explicit override (CI, or a runtime assembled locally by scripts/sdk/assemble-runtime-linux.sh).
  const override = process.env.SOLID_QML_RUNTIME_DIR;
  if (override) {
    const rt = readManifest(path.resolve(override));
    if (!rt) throw new CliError(`SOLID_QML_RUNTIME_DIR=${override} has no runtime.json`);
    return rt;
  }
  const name = `@solid-qml/runtime-${key}`;
  for (const from of [path.join(projectDir, "package.json"), path.join(PACKAGE_DIR, "package.json")]) {
    try {
      const pkgJson = createRequire(from).resolve(`${name}/package.json`);
      const rt = readManifest(path.dirname(pkgJson));
      if (rt) return rt;
    } catch { /* not installed from here */ }
  }
  const isHost = key === hostKey();
  throw new CliError(
    `the native runtime package ${name} is not installed` +
      (isHost
        ? ` (it is an optional dependency of solid-qml — reinstall without --no-optional / --omit=optional)`
        : ` — this host is ${hostKey()}; add the target runtime explicitly: npm i -D ${name} --force`),
  );
}
