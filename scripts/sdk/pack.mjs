#!/usr/bin/env node
// `npm run sdk:pack [-- <out-dir>]` — build the JS payloads (scripts/sdk/build-packages.mjs) and
// `npm pack` the three SDK packages into <out-dir> (default .scratch/sdk-pack). The runtime package
// needs its payload first: scripts/sdk/build-runtime-linux.sh. Nothing is published.
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const out = path.resolve(process.argv[2] ?? path.join(root, ".scratch/sdk-pack"));
mkdirSync(out, { recursive: true });

execFileSync(process.execPath, [path.join(root, "scripts/sdk/build-packages.mjs")], { stdio: "inherit" });
if (!existsSync(path.join(root, "packages/runtime-linux-x64/runtime.json"))) {
  console.error("sdk:pack: packages/runtime-linux-x64 has no payload — run scripts/sdk/build-runtime-linux.sh first");
  process.exit(1);
}
for (const p of ["create-solid-qml", "solid-qml", "runtime-linux-x64"]) {
  const res = execFileSync("npm", ["pack", "--json", "--pack-destination", out], { cwd: path.join(root, "packages", p), encoding: "utf8" });
  const parsed = JSON.parse(res); // npm <= 10: [info]; npm 11+: { "<name>": info }
  const info = Array.isArray(parsed) ? parsed[0] : Object.values(parsed)[0];
  console.log(`sdk:pack: ${path.relative(root, path.join(out, info.filename))} (${(info.size / 1048576).toFixed(1)} MB packed, ${info.entryCount} files)`);
}
