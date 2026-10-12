// `solid-qml doctor` — what this machine has for developing and packaging solid-qml apps:
// Node, glibc vs the runtime's floor, the runtime package(s) and their versions, unresolved shared
// libraries (ldd through the runtime launcher's own library setup, so bundled fallbacks count),
// and the packaging tools.
import { execFileSync, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";
import { TOOLS, cacheDir } from "./tools.mjs";
import { loadProject, PACKAGE_DIR } from "./project.mjs";
import { findRuntime, hostKey } from "./runtime.mjs";
import { readFileSync } from "node:fs";

const ok = (s) => console.log(`  ok    ${s}`);
const warn = (s) => console.log(`  warn  ${s}`);
const bad = (s) => console.log(`  FAIL  ${s}`);

const versionLt = (a, b) => {
  const pa = String(a).split(".").map(Number), pb = String(b).split(".").map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] ?? 0) !== (pb[i] ?? 0)) return (pa[i] ?? 0) < (pb[i] ?? 0);
  }
  return false;
};

export async function doctor(opts) {
  let failures = 0;
  const self = JSON.parse(readFileSync(path.join(PACKAGE_DIR, "package.json"), "utf8"));
  console.log(`solid-qml ${self.version} — doctor\n`);

  console.log("host");
  ok(`node ${process.version} on ${hostKey()}`);
  const glibc = process.report?.getReport?.().header?.glibcVersionRuntime;
  if (process.platform === "linux") glibc ? ok(`glibc ${glibc}`) : warn("glibc not detected (musl? the Linux runtime needs glibc)");

  console.log("\nproject");
  let projectDir = opts.cwd;
  try {
    const p = await loadProject(opts.cwd);
    projectDir = p.dir;
    ok(`${p.name}@${p.version} — entry ${path.relative(p.dir, p.entry)}, appId ${p.appId}`);
  } catch (err) {
    warn(`no app here (${err.message})`);
  }

  console.log("\nnative runtime");
  let runtime;
  try {
    runtime = findRuntime(projectDir);
    const m = runtime.manifest;
    ok(`${m.name}@${m.version} at ${runtime.dir}`);
    ok(`Qt ${m.qt}, built on ${m.builtOn} (${m.compiler}), solid-qml ${m.solidQml} @ ${m.commit}`);
    if (glibc && m.glibc && versionLt(glibc, m.glibc)) { bad(`glibc ${glibc} < required ${m.glibc}`); failures++; }
    else if (m.glibc) ok(`glibc floor ${m.glibc} satisfied`);
    if (m.version !== self.version) warn(`runtime ${m.version} != solid-qml ${self.version} (keep them in lockstep)`);
  } catch (err) {
    bad(err.message);
    failures++;
  }

  if (runtime && process.platform === "linux") {
    const res = spawnSync(runtime.launcher, [], { env: { ...process.env, SOLID_QML_LAUNCHER_INFO: "1" }, encoding: "utf8" });
    if (res.error) { bad(`cannot run ${runtime.launcher}: ${res.error.message}`); failures++; }
    else {
      const out = `${res.stdout}${res.stderr}`;
      const fallbacks = [...out.matchAll(/^fallback (\S+)/gm)].map((m) => m[1]);
      fallbacks.length
        ? ok(`host lacks ${fallbacks.length} desktop libs — using the runtime's bundled copies: ${fallbacks.join(" ")}`)
        : ok("all desktop system libraries come from the host");
      const missing = new Map();
      let current = "";
      for (const line of out.split("\n")) {
        const head = line.match(/^ldd (\S+)/);
        if (head) { current = head[1]; continue; }
        const nf = line.match(/^\s*(\S+) => not found/);
        if (nf) missing.set(nf[1], [...(missing.get(nf[1]) ?? []), current]);
      }
      const loaderMissing = [...missing].filter(([, who]) => who.some((w) => w.endsWith("solid-qml-loader")));
      for (const [lib, who] of missing) {
        const isLoader = who.some((w) => w.endsWith("solid-qml-loader"));
        (isLoader ? bad : warn)(`${lib} not found (needed by ${who.map((w) => path.basename(w)).join(", ")})`);
      }
      if (loaderMissing.length) failures++;
      else if (!missing.size) ok("ldd: every shared library of the loader and platform plugins resolves");
      if (/^fonts none/m.test(out)) warn("no fonts installed on the host — the runtime falls back to its bundled font");
    }
  }

  console.log("\npackaging (build --target linux)");
  const tar = spawnSync("tar", ["--version"], { encoding: "utf8" });
  tar.error ? (bad("tar not found (needed for the .tar.gz and to stage the AppImage)"), failures++) : ok("tar");
  for (const [name, t] of Object.entries(TOOLS)) {
    existsSync(path.join(cacheDir(), "tools", t.file))
      ? ok(`${name} cached (${t.file})`)
      : ok(`${name} will be downloaded on first build (${t.url}, sha256-pinned)`);
  }

  console.log(failures ? `\n${failures} problem(s) found.` : "\nall good.");
  return failures ? 1 : 0;
}
