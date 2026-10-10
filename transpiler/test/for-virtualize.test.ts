// Step 5v-1 (virtualized CssRepeater), Task 1: transpiler certification of <For> rows.
// See docs/superpowers/plans/2026-10-10-step5v1-virtual-repeater.md ("5v contract" in
// docs/superpowers/plans/2026-10-09-native-beats-web-roadmap.md). Emission is automatic for a
// certified row (Task 2 removed the SQ_VIRTUALIZE gate once the engine learned `virtualize`): a
// certified row gets `virtualize: true` right after `model:`, and a non-certified row emits exactly
// what it emitted before the feature existed (no `virtualize` line, nothing else changed).
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

async function assertCertified(src: string): Promise<string> {
  const out = await qml(src);
  assert.match(out, /^\s*model: .*\n\s*virtualize: true$/m, out);
  return out;
}

/** A row that is NOT certified emits no `virtualize` line — its output is what the transpiler
 *  produced before 5v-1 (the line is the feature's only emission change). */
async function assertNotCertified(src: string): Promise<string> {
  const out = await qml(src);
  assert.doesNotMatch(out, /virtualize/, out);
  return out;
}

// ---- No gate: emission is automatic, and SQ_VIRTUALIZE no longer changes anything --------------

test("no gate: a certified row emits virtualize with or without SQ_VIRTUALIZE in the environment", async () => {
  const src = `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div class="bench-row"><text class="bench-id">{row.id}</text><text class="bench-label">{row.label}</text></div>}</For>; }`;
  const prev = process.env.SQ_VIRTUALIZE;
  try {
    delete process.env.SQ_VIRTUALIZE;
    const unset = await qml(src);
    process.env.SQ_VIRTUALIZE = "0";
    const zero = await qml(src);
    assert.match(unset, /virtualize: true/, unset);
    assert.equal(zero, unset);
  } finally {
    if (prev === undefined) delete process.env.SQ_VIRTUALIZE; else process.env.SQ_VIRTUALIZE = prev;
  }
});

// ---- Certified --------------------------------------------------------------------------------

test("certified: the bench row (div.bench-row > text.bench-id + text.bench-label)", async () => {
  const out = await assertCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div class="bench-row"><text class="bench-id">{row.id}</text><text class="bench-label">{row.label}</text></div>}</For>; }`,
  );
  assert.match(out, /text: "" \+ \(modelData\.id\)/);
  assert.match(out, /text: "" \+ \(modelData\.label\)/);
});

test("certified: a row with a dynamic class (member access of the row param)", async () => {
  await assertCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div class={row.status}><text>{row.id}</text></div>}</For>; }`,
  );
});

test("certified: a row with a template literal", async () => {
  const out = await assertCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><text>{` + "`id:${row.id}`" + `}</text>}</For>; }`,
  );
  assert.match(out, /text: "" \+ \("id:" \+ \(modelData\.id\)\)/);
});

test("certified: the bare row param as text content (today's <text>{item}</text> shape)", async () => {
  await assertCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><text>{row}</text>}</For>; }`,
  );
});

// ---- NOT certified — output byte-identical to the gate being off ------------------------------

test("not certified: onClick on the row", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div class="bench-row" onClick={() => setRows(rows())}><text>{row.id}</text></div>}</For>; }`,
  );
});

test("not certified: a ref on the row", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); let box; return <For each={rows()}>{(row)=><div ref={box}><text>{row.id}</text></div>}</For>; }`,
  );
});

test("not certified: a capitalized component/builtin tag", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><Calendar/>}</For>; }`,
  );
});

test("not certified: a nested <For> disqualifies the OUTER row (an independently-passive INNER row may still certify on its own)", async () => {
  const src = `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div><For each={row.items}>{(it)=><text>{it}</text>}</For></div>}</For>; }`;
  const out = await qml(src);
  // The outer repeater's own `model:` line gets no virtualize right after it...
  assert.doesNotMatch(out, /model: rows\n\s*virtualize: true/, out);
  // ...only the inner repeater (a wholly separate, independently passive <For>) does — exactly once.
  assert.match(out, /model: __for0\.__forItem\.items\n\s*virtualize: true/, out);
  assert.equal(out.match(/virtualize: true/g)?.length, 1, out);
});

test("not certified: a nested <Show>", async () => {
  await assertNotCertified(
    `import { For, Show } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div><Show when={row.on}><text>hi</text></Show></div>}</For>; }`,
  );
});

test("not certified: a call in the row body", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); function fmt(x) { return x; } return <For each={rows()}>{(row)=><text>{fmt(row.x)}</text>}</For>; }`,
  );
});

test("not certified: a spread onto the row", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div {...row}><text>x</text></div>}</For>; }`,
  );
});

test("not certified: a signal read inside the row", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); const [count,setCount]=createSignal(0); return <For each={rows()}>{(row)=><text>{count()}</text>}</For>; }`,
  );
});

test("not certified: a button element", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><button>{row.label}</button>}</For>; }`,
  );
});

test("not certified: an identifier from the outer (closed-over) scope", async () => {
  await assertNotCertified(
    `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); const prefix = "x: "; return <For each={rows()}>{(row)=><text>{prefix + row.x}</text>}</For>; }`,
  );
});
