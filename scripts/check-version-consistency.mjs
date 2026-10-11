#!/usr/bin/env node
// Version-consistency gate (release-plan M8): meson.build's project() version is the single
// source of truth for the app's version. Everything else that reports or records a version
// (package.json, package-lock.json, the Sphinx doc sites) must say exactly the same string.
// The engine (vendor/qml-css-engine) is NOT checked here — it deliberately keeps its own semver
// (docs/release-policy.md); the app pins its submodule SHA instead of its version.
//
// Run directly: `node scripts/check-version-consistency.mjs`
// Wired into `meson test` as the `version-consistency` test (see meson.build).

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const read = (p) => readFileSync(path.join(root, p), "utf8");

function fail(message) {
  console.error(`version-consistency: FAIL — ${message}`);
  process.exitCode = 1;
}

// 1. The source of truth: the literal `version: '...'` on meson's project() call. It is the
// first single-quoted `version:` literal in the file — shared_library()'s own `version:`/
// `soversion:` further down are the library's ABI filename suffix, a deliberately DIFFERENT,
// purely numeric string (docs/release-policy.md), not the project version.
const mesonBuild = read("meson.build");
const mesonMatch = mesonBuild.match(/version:\s*'([^']+)'/);
if (!mesonMatch) {
  fail("could not find a literal version: '...' in meson.build");
  process.exit(process.exitCode ?? 1);
}
const appVersion = mesonMatch[1];
console.log(`version-consistency: source of truth (meson.build project version) = ${appVersion}`);

// Sanity check: the runtime-reported version (process.versions.solidQml) must be DERIVED from
// meson.project_version(), never a second hardcoded literal.
if (!/add_project_arguments\(\s*'-DSOLID_QML_VERSION="'\s*\+\s*meson\.project_version\(\)\s*\+\s*'"'/.test(mesonBuild)) {
  fail("SOLID_QML_VERSION is no longer derived from meson.project_version() in meson.build");
}

// 2 & 3. package.json / package-lock.json.
const pkg = JSON.parse(read("package.json"));
if (pkg.version !== appVersion) {
  fail(`package.json version is "${pkg.version}", expected "${appVersion}"`);
}

const lock = JSON.parse(read("package-lock.json"));
if (lock.version !== appVersion) {
  fail(`package-lock.json root version is "${lock.version}", expected "${appVersion}"`);
}
const lockRootPkg = lock.packages?.[""];
if (!lockRootPkg || lockRootPkg.version !== appVersion) {
  fail(`package-lock.json packages[""].version is "${lockRootPkg?.version}", expected "${appVersion}"`);
}

// 4 & 5. The two Sphinx doc sites (docs/conf.py, docs/v4-engine/conf.py).
for (const confPath of ["docs/conf.py", "docs/v4-engine/conf.py"]) {
  const conf = read(confPath);
  const releaseMatch = conf.match(/^release\s*=\s*"([^"]+)"/m);
  if (!releaseMatch) {
    fail(`${confPath}: no release = "..." line found`);
    continue;
  }
  if (releaseMatch[1] !== appVersion) {
    fail(`${confPath} release is "${releaseMatch[1]}", expected "${appVersion}"`);
  }
}

if (process.exitCode) {
  console.error("version-consistency: one source of truth, meson.build project version — fix the files above to match it.");
  process.exit(process.exitCode);
}

console.log("version-consistency: OK — package.json, package-lock.json, docs/conf.py and docs/v4-engine/conf.py all match meson.build.");
