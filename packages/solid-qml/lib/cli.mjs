// solid-qml CLI: create | dev | build | doctor.
import path from "node:path";
import { readFileSync } from "node:fs";
import { CliError, PACKAGE_DIR } from "./project.mjs";

const USAGE = `solid-qml — Solid.js apps rendered natively with Qt Quick

Usage:
  solid-qml create <dir>                 scaffold a new app (same as: npm create solid-qml <dir>)
  solid-qml dev [--grab out.png] [-- <loader args>]
                                         native window with hot reload (the browser preview is \`vite\`)
  solid-qml build --target linux [--out-dir dist/linux] [--no-appimage]
                                         package the app: AppImage + portable .tar.gz
  solid-qml doctor                       check the runtime, system libraries and packaging tools
  solid-qml --version

Environment:
  SOLID_QML_RUNTIME_DIR   use this runtime package directory instead of @solid-qml/runtime-<platform>
  SOLID_QML_CACHE         where pinned packaging tools are cached (default ~/.cache/solid-qml)
`;

function parse(argv) {
  const opts = { _: [], loaderArgs: [], cwd: process.cwd() };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--") { opts.loaderArgs = argv.slice(i + 1); break; }
    if (a === "--target" || a === "-t") opts.target = argv[++i];
    else if (a.startsWith("--target=")) opts.target = a.slice(9);
    else if (a === "--out-dir") opts.outDir = argv[++i];
    else if (a === "--grab") opts.grab = argv[++i];
    else if (a === "--cwd") opts.cwd = path.resolve(argv[++i]);
    else if (a === "--no-appimage") opts.appimage = false;
    else if (a === "--template") opts.template = argv[++i];
    else if (a === "-h" || a === "--help") opts.help = true;
    else if (a === "-v" || a === "--version") opts.version = true;
    else if (a.startsWith("-")) throw new CliError(`unknown option ${a}\n\n${USAGE}`);
    else opts._.push(a);
  }
  return opts;
}

export async function main(argv) {
  try {
    const opts = parse(argv);
    if (opts.version) {
      console.log(JSON.parse(readFileSync(path.join(PACKAGE_DIR, "package.json"), "utf8")).version);
      return 0;
    }
    const [cmd, ...rest] = opts._;
    if (opts.help || !cmd) { console.log(USAGE); return cmd || opts.help ? 0 : 1; }
    switch (cmd) {
      case "create": {
        const { scaffold } = await import("create-solid-qml");
        await scaffold({ dir: rest[0], template: opts.template });
        return 0;
      }
      case "dev": {
        const { dev } = await import("./dev.mjs");
        await dev(opts);
        return null; // stays alive with the loader
      }
      case "build": {
        const target = opts.target ?? (process.platform === "linux" ? "linux" : undefined);
        if (target !== "linux") throw new CliError(`unsupported --target ${target ?? "(none)"} — this preview packages: linux`);
        const { buildLinux } = await import("./build-linux.mjs");
        await buildLinux(opts);
        return 0;
      }
      case "doctor": {
        const { doctor } = await import("./doctor.mjs");
        return await doctor(opts);
      }
      default:
        throw new CliError(`unknown command "${cmd}"\n\n${USAGE}`);
    }
  } catch (err) {
    if (err instanceof CliError) { console.error(`solid-qml: ${err.message}`); return 1; }
    throw err;
  }
}
