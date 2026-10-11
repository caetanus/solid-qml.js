// C23 (owner decision 2026-10-11): <For> row identity is the element's REFERENCE by default (Solid web);
// `key="field"` / `key={(r) => r.field}` opts into reuse by that field. The transpiler's whole part is
// emitting the repeater's `key` (no line by default) and rejecting what it cannot honour.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { generateCpp } from "../src/emit/cpp/index.ts";
import type { Scope } from "../src/emit/expr.ts";

async function qml(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const render = findRender(ast)!;
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding" };
  return emitQml(render, scope).join("\n");
}

const forWith = (attr: string) => `import { createSignal, For } from "solid-js";
  export function C(){ const [rows,setRows]=createSignal([]);
    return <div><For each={rows()} ${attr}>{(row)=><text>{row.label}</text>}</For></div>; }`;

test("default <For>: no key line (reference identity is the engine's default)", async () => {
  const out = await qml(forWith(""));
  assert.match(out, /Css\.CssRepeater \{/);
  assert.doesNotMatch(out, /^\s*key:/m, out);
});

test("key=\"id\" → the repeater's key, right after model:", async () => {
  const out = await qml(forWith(`key="id"`));
  assert.match(out, /^\s*model: .*\n\s*key: "id"$/m, out);
});

test("key={`uid`} (no-expression template) is a literal field name too", async () => {
  assert.match(await qml(forWith("key={`uid`}")), /^\s*key: "uid"$/m);
});

test("key as a field accessor: (r) => r.id, (r) => r[\"id\"], function (r) { return r.id }", async () => {
  for (const attr of [`key={(r) => r.id}`, `key={(r) => r["id"]}`, `key={function (r) { return r.id; }}`])
    assert.match(await qml(forWith(attr)), /^\s*key: "id"$/m, attr);
});

test("a computed key function / a dynamic key / a non-field string fail loud", async () => {
  for (const attr of [`key={(r) => r.a + r.b}`, `key={(r) => String(r.id)}`, `key={(r) => r.user.id}`,
                      `key={(r, i) => r.id}`, `key={(r) => other.id}`, `key={rows()}`, `key="a.b"`, `key=""`]) {
    await assert.rejects(() => qml(forWith(attr)), /<For> key must be a field name .*key=\{\(r\) => r\.id\}/, attr);
  }
});

test("key composes with virtualize (certified passive row)", async () => {
  const out = await qml(`import { createSignal, For } from "solid-js";
    export function C(){ const [rows,setRows]=createSignal([]);
      return <For each={rows()} key="id">{(row)=><div class="r"><text>{row.label}</text></div>}</For>; }`);
  assert.match(out, /^\s*model: .*\n\s*key: "id"\n\s*virtualize: true$/m, out);
});

test("AOT C++: <For key> fails loud (no reconciler there, C7)", async () => {
  const src = `import { createSignal, For } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
export function App(){ const [rows,setRows]=createSignal([{ id: 1, label: "a" }]);
  return <Window title="t" width={200} height={100} visible><div class="a"><For each={rows()} key="id">{(r) => <text>{r.label}</text>}</For></div></Window>; }`;
  await assert.rejects(() => generateCpp(src, "k.tsx"), /AOT: <For key> is not supported/);
  // ...and without key the same app still emits.
  await generateCpp(src.replace(` key="id"`, ""), "k.tsx");
});

test("<For key> where rows are not repeater rows (<select> options, menu items) fails loud", async () => {
  await assert.rejects(() => qml(`import { createSignal, For } from "solid-js";
    export function C(){ const [o] = createSignal([]);
      return <select><For each={o()} key="id">{(x) => <option value={x.id}>{x.label}</option>}</For></select>; }`),
    /<select><For key> is not supported/);
  await assert.rejects(() => qml(`import { createSignal, For } from "solid-js";
    export function C(){ let r; const [s] = createSignal([]);
      return <Menu ref={r}><For each={s()} key="id">{(w) => <MenuItem>{w.label}</MenuItem>}</For></Menu>; }`),
    /<Menu><For key> is not supported/);
});
