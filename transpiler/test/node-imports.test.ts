import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { parseSync } from "@babel/core";
import { generate } from "../src/index.ts";
import { emitV4Module } from "../src/resolve/v4-compat.ts";
import { isShimmedNodeBuiltin, isNodeBuiltinSpecifier } from "../src/resolve/node.ts";

const parseModule = (code: string) =>
  parseSync(code, { sourceType: "module", babelrc: false, configFile: false })!;

test("nodeimports: mirrors real npm ESM tree and wires imported values into QML", async () => {
  const f = fileURLToPath(new URL("../../examples/nodeimports.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  const moduleEntries = Object.entries(app.modules);
  const moduleSources = moduleEntries.map(([, source]) => source);

  assert.ok(moduleEntries.length >= 8, "wrap-ansi dependency tree is mirrored");
  assert.ok(moduleEntries.every(([name]) => name.startsWith("modules/") && name.endsWith(".mjs")));
  assert.ok(moduleSources.every((source) => !/\bfrom\s+["'](?!\.\/)/.test(source)), "mirrored imports are relative");
  assert.ok(moduleSources.every((source) => !/\/[gimsuy]*v[gimsuy]*\b/.test(source)), "regexp v flag is lowered");
  assert.ok(moduleSources.every((source) => !/\.at\(-1\)/.test(source)), "Array.prototype.at(-1) is lowered");
  assert.ok(moduleSources.every((source) => !/replaceAll\(/.test(source)), "String.prototype.replaceAll is lowered");
  assert.ok(moduleSources.every((source) => !/[^.]\.{3}[A-Za-z_$[{]/.test(source)), "spread syntax is lowered");
  assert.ok(moduleSources.some((source) => /export \{ wrapAnsi as Default \}/.test(source)), "default export is exposed as named Default");
  assert.ok(moduleSources.some((source) => /def\(String\.prototype, "trimStart"/.test(source)), "pure ECMAScript polyfills are injected in mirrored modules");
  assert.ok(moduleSources.every((source) => !/var TextEncoder = undefined/.test(source)), "text encoding is a host shim, not mirror code");

  assert.equal(moduleSources.filter((source) => /export default function stripAnsi\b/.test(source)).length, 1);
  assert.equal(moduleSources.filter((source) => /export default function ansiRegex\b/.test(source)).length, 1);

  assert.match(app.entry, /import "modules\/base64_[a-f0-9]+\.mjs" as Js0/);
  assert.match(app.entry, /import "modules\/index_[a-f0-9]+\.mjs" as Js1/);
  assert.match(app.entry, /readonly property var encoded: Js0\.Base64\.encode\("solid-qml"\)/);
  assert.match(app.entry, /readonly property var decoded: Js0\.Base64\.decode\(encoded\)/);
  assert.match(app.entry, /readonly property var wrapped: Js1\.Default\(/);
  assert.doesNotMatch(app.entry, /Component\.onCompleted: \{ var encoded/);
});

test("nodeimports: TypedArray .apply call sites are normalized for the V4 spread bug", async () => {
  const f = fileURLToPath(new URL("../../examples/nodeimports.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  const sources = Object.values(app.modules);
  // V4's Function.prototype.apply reads a TypedArray's length but every element as 0, so js-base64's
  // `String.fromCharCode.apply(null, uint8array)` would produce zero bytes. apply is frozen and can't
  // be patched, so each `.apply(thisArg, args)` arg list is wrapped to slice a TypedArray view into a
  // real array, with the helper defined once per module that needs it.
  const patched = sources.filter((s) => /\.apply\([^,]*,\s*__v4ApplySpread\(/.test(s));
  assert.ok(patched.length >= 1, "mirrored .apply call sites are wrapped with __v4ApplySpread");
  assert.ok(patched.every((s) => /function __v4ApplySpread\(/.test(s)), "modules using the normalizer also define it");
});

test("v4-compat: free XMLHttpRequest references are rewritten to the fetch-backed global", () => {
  const out = emitV4Module(parseModule(
    "export function make(){ return new XMLHttpRequest(); }\n" +
    "export const supported = typeof XMLHttpRequest !== 'undefined';",
  ));
  assert.match(out, /new __SqXMLHttpRequest\(\)/, "constructor call is rewritten");
  assert.match(out, /typeof __SqXMLHttpRequest/, "feature-detection is rewritten");
});

test("v4-compat: a locally-bound XMLHttpRequest is left untouched", () => {
  const out = emitV4Module(parseModule(
    "export function f(XMLHttpRequest){ return new XMLHttpRequest(); }",
  ));
  assert.doesNotMatch(out, /__SqXMLHttpRequest/, "a local binding is not treated as the global");
});

test("v4-compat: RegExp construction is routed through __SqRegExp (named-group .groups fix)", () => {
  const out = emitV4Module(parseModule(
    "export const re = new RegExp('(?<sgr>[0-9]+)m');\n" +
    "export const fn = RegExp('x');",
  ));
  assert.match(out, /new __SqRegExp\(/, "new RegExp(...) is rewritten");
  assert.match(out, /__SqRegExp\('x'\)/, "RegExp(...) call form is rewritten");
});

test("v4-compat: a locally-bound RegExp is left untouched", () => {
  const out = emitV4Module(parseModule("export function f(RegExp){ return new RegExp('x'); }"));
  assert.doesNotMatch(out, /__SqRegExp/, "a local binding is not treated as the global RegExp");
});

test("nodeimports: fs / child_process / process are shimmed builtins (host-provided, not rejected)", () => {
  for (const spec of ["fs", "node:fs", "fs/promises", "child_process", "node:child_process", "process"])
    assert.ok(isShimmedNodeBuiltin(spec), `${spec} is shimmed`);
  // still real builtins, just provided at runtime rather than mirrored
  assert.ok(isNodeBuiltinSpecifier("fs") && isNodeBuiltinSpecifier("child_process"));
  // unshimmed builtins remain unsupported
  for (const spec of ["path", "node:os", "crypto", "stream"])
    assert.ok(!isShimmedNodeBuiltin(spec), `${spec} is NOT shimmed`);
});

test("nodeimports: an unshimmed builtin (node:path) is still rejected", async () => {
  await assert.rejects(
    () => generate(
      `import path from "node:path";
       export function App(){ return <text>{path.sep}</text>; }`,
      "builtin.tsx",
    ),
    /Node builtin import is not supported/,
  );
});

test("nodeimports: rejects Node builtin imports instead of dropping them", async () => {
  await assert.rejects(
    () => generate(
      `import path from "node:path";
       export function App(){ return <text>{path.sep}</text>; }`,
      "builtin.tsx",
    ),
    /Node builtin import is not supported/,
  );
});
