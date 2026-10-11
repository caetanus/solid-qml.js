// Regression tests for the <For> index-accessor miscompile: in Solid, a <For> delegate's SECOND
// parameter is an INDEX ACCESSOR — `(item, i) => …` with `i()` reading the row's CURRENT index —
// unlike <Index>, where the second param is a plain number. The transpiler only ever read
// `delegate.params[0]` (rowDelegate in emit/qml.ts): the index param was silently dropped, so every
// reference to it survived in the emitted text as a bare identifier with no QML meaning —
// `ReferenceError: i is not defined` at runtime, found in examples/document-editor (outline
// highlighting: `doc.currentHeadingIndex === i() ? "on" : ""`).
//
// Fix: rowDelegate (shared by <For> and <VirtualList> — both build their row via the same
// function) now also reads `delegate.params[1]`, routing it through `scope.accessors` (the same
// mechanism <Index> already uses for its row-VALUE accessor): a call `i()` resolves to the fixed
// QML `index` expression; a bare (uncalled) reference — `i` passed as a value, e.g.
// `onClick={() => select(i)}` — resolves to a closure that reads it, since Solid's `i` IS itself
// an accessor function and code may pass it around before calling it.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
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

test("For index accessor in a class/attribute position", async () => {
  const out = await qml(
    `import { For } from "solid-js"; export function C(){ const [hs,setHs]=createSignal([]); const [cur,setCur]=createSignal(1); return <For each={hs()}>{(h, i) => <div class={cur() === i() ? "on" : "off"}><text>{h.text}</text></div>}</For>; }`,
  );
  assert.doesNotMatch(out, /\bi\(\)/, out);
  assert.doesNotMatch(out, /(?<![\w.])\bi\b(?!ndex)/, out); // no stray bare `i` identifier anywhere
  assert.match(out, /cur === index \? "on" : "off"/, out);
});

test("For index accessor in text position", async () => {
  const out = await qml(
    `import { For } from "solid-js"; export function C(){ const [hs,setHs]=createSignal([]); return <For each={hs()}>{(h, i) => <text>{h.text}-{i()}</text>}</For>; }`,
  );
  assert.match(out, /text: "" \+ \(modelData\.text\) \+ "-" \+ \(index\)/, out);
});

test("For index accessor called inside a handler captures the CURRENT index at event time", async () => {
  const out = await qml(
    `export function C(){ const [hs,setHs]=createSignal([]); function select(n){} return <For each={hs()}>{(h, i) => <div onClick={() => select(i())}>{h.text}</div>}</For>; }`,
  );
  assert.match(out, /onClicked: select\(index\)/, out);
});

test("For: a bare index accessor passed as a VALUE (not called) becomes a closure that reads it", async () => {
  const out = await qml(
    `export function C(){ const [hs,setHs]=createSignal([]); function select(n){} return <For each={hs()}>{(h, i) => <div onClick={() => select(i)}>{h.text}</div>}</For>; }`,
  );
  assert.match(out, /onClicked: select\(\(function\(\) \{ return index \}\)\)/, out);
});

test("For without the index param is emitted exactly as before (no accessor plumbing)", async () => {
  const out = await qml(
    `import { For } from "solid-js"; export function C(){ const [hs,setHs]=createSignal([]); return <For each={hs()}>{(h) => <text>{h.text}</text>}</For>; }`,
  );
  assert.doesNotMatch(out, /__forIndex|\bindex\b/, out);
  assert.match(out, /text: "" \+ \(modelData\.text\)/, out);
});

test("For index used inside a NESTED <For>: it is published (like the row item) and reachable by id", async () => {
  const out = await qml(
    `import { For } from "solid-js"; export function C(){ const outer=[]; return <For each={outer}>{(row, i) => <div><For each={row.cols}>{(col, j) => <text>{col}-{j()}-{i()}</text>}</For></div>}</For>; }`,
  );
  const id = out.match(/id: (__for\d+)\n\s*property var __forItem: modelData\n\s*property int __forIndex: index/);
  assert.ok(id, out);
  assert.match(out, new RegExp(`text: "" \\+ \\(modelData\\) \\+ "-" \\+ \\(index\\) \\+ "-" \\+ \\(${id![1]}\\.__forIndex\\)`), out);
});

test("For: a nested <For> reusing the SAME index name shadows the outer one (real JS lexical scoping)", async () => {
  const out = await qml(
    `import { For } from "solid-js"; export function C(){ const outer=[]; return <For each={outer}>{(row, i) => <div><For each={row.cols}>{(col, i) => <text>{col}-{i()}</text>}</For></div>}</For>; }`,
  );
  // The INNER i() must resolve to the inner loop's own bare `index`, never the outer's published id.
  assert.match(out, /text: "" \+ \(modelData\) \+ "-" \+ \(index\)/, out);
});

test("VirtualList shares rowDelegate: its index accessor positions resolve the same way", async () => {
  const out = await qml(
    `import { VirtualList } from "solid-js"; export function C(){ const [ts,setTs]=createSignal([]); return <VirtualList class="rows" each={ts()}>{(t, i) => <div class={i() === 0 ? "first" : ""}><text>{t.from}-{i()}</text></div>}</VirtualList>; }`,
  );
  assert.match(out, /index === 0 \? "first" : ""/, out);
  assert.match(out, /text: "" \+ \(modelData\.from\) \+ "-" \+ \(index\)/, out);
});
