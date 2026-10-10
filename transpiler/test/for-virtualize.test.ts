// Step 5v-1 (virtualized CssRepeater), Task 1: transpiler certification of <For> rows.
// See docs/superpowers/plans/2026-10-10-step5v1-virtual-repeater.md ("5v contract" in
// docs/superpowers/plans/2026-10-09-native-beats-web-roadmap.md). The engine does not understand
// `virtualize` yet (Task 2), so emission is gated behind SQ_VIRTUALIZE=1 (default OFF): with the
// gate off, a certified row's emission must be byte-identical to a non-certified row's.
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

/** Runs `qml(src)` with SQ_VIRTUALIZE forced to "1" or removed, restoring the previous value
 *  afterwards (tests in this file run sequentially, but this keeps each test self-contained). */
async function qmlGated(src: string, on: boolean): Promise<string> {
  const prev = process.env.SQ_VIRTUALIZE;
  if (on) process.env.SQ_VIRTUALIZE = "1"; else delete process.env.SQ_VIRTUALIZE;
  try {
    return await qml(src);
  } finally {
    if (prev === undefined) delete process.env.SQ_VIRTUALIZE; else process.env.SQ_VIRTUALIZE = prev;
  }
}

async function assertCertified(src: string): Promise<string> {
  const out = await qmlGated(src, true);
  assert.match(out, /^\s*model: .*\n\s*virtualize: true$/m, out);
  return out;
}

/** A row that is NOT certified must emit IDENTICALLY whether the gate is on or off — i.e. the
 *  feature never changes its output. */
async function assertNotCertified(src: string): Promise<string> {
  const off = await qmlGated(src, false);
  const on = await qmlGated(src, true);
  assert.doesNotMatch(off, /virtualize: true/, off);
  assert.equal(on, off);
  return off;
}

// ---- Gate itself -----------------------------------------------------------------------------

test("SQ_VIRTUALIZE gate: OFF by default, a certifiable row emits no virtualize line", async () => {
  const src = `import { For } from "solid-js"; export function C(){ const [rows,setRows]=createSignal([]); return <For each={rows()}>{(row)=><div class="bench-row"><text class="bench-id">{row.id}</text><text class="bench-label">{row.label}</text></div>}</For>; }`;
  const off = await qmlGated(src, false);
  assert.doesNotMatch(off, /virtualize: true/, off);
  const on = await qmlGated(src, true);
  assert.match(on, /virtualize: true/, on);
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
  const on = await qmlGated(src, true);
  const off = await qmlGated(src, false);
  // The outer repeater's own `model:` line gets no virtualize right after it...
  assert.doesNotMatch(on, /model: rows\n\s*virtualize: true/, on);
  // ...only the inner repeater (a wholly separate, independently passive <For>) does.
  assert.match(on, /model: __for0\.__forItem\.items\n\s*virtualize: true/, on);
  assert.equal(on.replace(/\n\s*virtualize: true/, ""), off);
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
