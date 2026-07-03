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

test("Show: a child element gets a visible guard from when", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [ok,setOk]=createSignal(true); return <Show when={ok()}><text>hi</text></Show>; }`);
  assert.match(out, /Css\.CssText \{[\s\S]*visible: !!\(ok\)[\s\S]*text: "hi"/);
});

test("Show: with a wrapper element, only the wrapper carries the guard", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [ok,setOk]=createSignal(true); return <Show when={ok()}><div class="box"><text>hi</text></div></Show>; }`);
  assert.match(out, /Css\.CssRect \{[\s\S]*visible: !!\(ok\)/);
});

test("For: becomes a Repeater whose delegate binds item to modelData", async () => {
  const out = await qml(`import { For } from "solid-js"; export function C(){ const [items,setItems]=createSignal([]); return <For each={items()}>{(item)=><text>{item}</text>}</For>; }`);
  assert.match(out, /Repeater \{/);
  assert.match(out, /model: items/);
  assert.match(out, /Css\.CssText \{[\s\S]*text: "" \+ \(modelData\)/);
});

test("Switch: each Match branch is LAZY and ASYNC (a CssIncubator gated by its when minus the priors)", async () => {
  const out = await qml(`import { Switch, Match } from "solid-js"; export function C(){ const [n,setN]=createSignal(0); return <Switch><Match when={n() === 0}><text>zero</text></Match><Match when={n() === 1}><text>one</text></Match></Switch>; }`);
  // Each branch INCUBATES its content only while its guard holds (async page mount).
  assert.match(out, /Css\.CssIncubator \{/);
  assert.match(out, /active: \(n === 0\) \? true : false[\s\S]*text: "zero"/);
  assert.match(out, /active: \(\(n === 1\) && !\(\(n === 0\)\)\) \? true : false[\s\S]*text: "one"/);
  // No eager visible-gating of Switch branches anymore.
  assert.doesNotMatch(out, /visible: !!\(n === 0\)/);
});

test("Show: fallback element is gated with the inverted guard", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [c,setC]=createSignal(true); return <Show when={c()} fallback={<text>none</text>}><text>yes</text></Show>; }`);
  // child must carry the positive guard
  assert.match(out, /text: "yes"[\s\S]*visible: !!\(c\)|visible: !!\(c\)[\s\S]*text: "yes"/);
  // fallback must carry the inverted guard
  assert.match(out, /text: "none"[\s\S]*visible: !\(c\)|visible: !\(c\)[\s\S]*text: "none"/);
  // child guard must be positive (not inverted)
  assert.doesNotMatch(out, /visible: !\(c\)[\s\S]{0,100}text: "yes"/);
  // fallback guard must NOT be the positive form
  assert.doesNotMatch(out, /visible: !!\(c\)[\s\S]{0,100}text: "none"/);
});

test("Dynamic over a text tag emits a CssText with a reactive cssPrimitive", async () => {
  const out = await qml(`import { Dynamic } from "solid-js/web"; export function C(){ const [tag,setTag]=createSignal("h1"); return <Dynamic component={tag()}>hi</Dynamic>; }`);
  assert.match(out, /Css\.CssText \{/);
  assert.match(out, /cssPrimitive: tag/);
  assert.match(out, /text: "hi"/);
});

test("Dynamic label text with angle brackets stays text, not a component tag", async () => {
  const out = await qml(`
    import { Dynamic } from "solid-js/web";
    export function C(){
      const [tag,setTag]=createSignal("h");
      return <button>render as: &lt;{tag()}&gt;</button>;
    }
  `);
  assert.match(out, /text: "render as: <" \+ \(tag\) \+ ">"/);
  assert.doesNotMatch(out, /unknown component/);
});

test("lowercase <h> is an intrinsic tag from Babel output, not a component", async () => {
  const out = await qml(`export function C(){ return <h class="headline">hi</h>; }`);
  assert.match(out, /Css\.CssRect \{/);
  assert.match(out, /cssPrimitive: "h"/);
  assert.match(out, /cssClass: \["headline"\]/);
});
