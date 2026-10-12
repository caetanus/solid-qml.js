#!/usr/bin/env node
// Builds the JS payload of the npm packages under packages/ from this repo's sources of truth:
//   transpiler/src            → packages/solid-qml/dist/transpiler.mjs (esbuild bundle; Babel stays an
//                               external npm dependency of the package)
//   src/solid-qml/{runtime.tsx,reset.css,base.css,jsx.d.ts,ambient-modules.d.ts,themes/}
//                             → packages/solid-qml/runtime/ (the authoring module apps import as "solid-qml")
//   LICENSE                   → packages/{solid-qml,create-solid-qml}/LICENSE
// The outputs are gitignored: the repo keeps ONE copy of each source; this script (run by
// `npm run sdk:build`, and by scripts/sdk/pack.mjs before `npm pack`) materialises the packages.
// The binary runtime payload (packages/runtime-linux-x64/runtime/) comes from
// scripts/sdk/assemble-runtime-linux.sh, not from here.
import { build } from "esbuild";
import { cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const pkgDir = path.join(root, "packages/solid-qml");
const runtimeSrc = path.join(root, "src/solid-qml");
const runtimeOut = path.join(pkgDir, "runtime");

// 1. Transpiler bundle. `packages: "external"` keeps every bare import (Babel and its plugins) as
// a runtime dependency resolved from the installed package — Babel's plugin objects are imported
// by value (transpiler/src/babel/transform.ts), so nothing is resolved from the APP's directory.
await rm(path.join(pkgDir, "dist"), { recursive: true, force: true });
await build({
  entryPoints: [path.join(root, "transpiler/src/index.ts")],
  outfile: path.join(pkgDir, "dist/transpiler.mjs"),
  bundle: true,
  platform: "node",
  format: "esm",
  target: "node20",
  packages: "external",
  sourcemap: false,
  legalComments: "inline",
  banner: { js: "// solid-qml transpiler (TSX → QML). Bundled from transpiler/src by scripts/sdk/build-packages.mjs." },
  logLevel: "warning",
});

// 2. Authoring runtime. index.tsx is runtime.tsx plus references to the JSX/ambient typings, so an
// app that imports "solid-qml" gets the intrinsic-element types (<text>, <Window>, native widgets)
// without a tsconfig "types" entry.
await rm(runtimeOut, { recursive: true, force: true });
await mkdir(runtimeOut, { recursive: true });
for (const f of ["reset.css", "base.css", "jsx.d.ts", "ambient-modules.d.ts"]) {
  await cp(path.join(runtimeSrc, f), path.join(runtimeOut, f));
}
await cp(path.join(runtimeSrc, "themes"), path.join(runtimeOut, "themes"), { recursive: true });
const runtimeTsx = await readFile(path.join(runtimeSrc, "runtime.tsx"), "utf8");
await writeFile(
  path.join(runtimeOut, "index.tsx"),
  [
    "/// <reference path=\"./jsx.d.ts\" />",
    "/// <reference path=\"./ambient-modules.d.ts\" />",
    "// The `solid-qml` authoring module (generated from src/solid-qml/runtime.tsx — edit that file).",
    "// Web: plain Solid components that render the same class/box structure as the native target.",
    "// Native: the transpiler compiles `import ... from \"solid-qml\"` away (QML types + loader context).",
    runtimeTsx,
  ].join("\n"),
);

// 3. License text for the JS packages (MIT, same as the repo).
for (const p of ["solid-qml", "create-solid-qml"]) {
  await cp(path.join(root, "LICENSE"), path.join(root, "packages", p, "LICENSE"));
}

console.log("[sdk:build] packages/solid-qml: dist/transpiler.mjs + runtime/ ready");
