import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import type { Scope } from "../src/emit/expr.ts";
import { generate } from "../src/index.ts";

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

test("Show: the child is LAZY — a Repeater mounts it only while `when` holds", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [ok,setOk]=createSignal(true); return <Show when={ok()}><text>hi</text></Show>; }`);
  assert.match(out, /Repeater \{\s*model: \(ok\) \? 1 : 0\s*W\.Text \{\s*text: "hi"/);
  assert.doesNotMatch(out, /visible: !!\(ok\)/); // not built-and-hidden any more
});

test("Show: a wrapper element is the delegate; its subtree comes and goes with it", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [ok,setOk]=createSignal(true); return <Show when={ok()}><div class="box"><text>hi</text></div></Show>; }`);
  assert.match(out, /Repeater \{\s*model: \(ok\) \? 1 : 0\s*W\.Div \{\s*cssClass: \["box"\]\s*W\.Text/);
  assert.equal(out.match(/Repeater \{/g)?.length, 1); // one gate, not one per descendant
});

test("Show: a ref inside is published on the component root while mounted", async () => {
  // Full component path: the root (emitComponentType) declares the holder property.
  const app = await generate(`import { createSignal, Show } from "solid-js"; export function C(){ let box; const [ok,setOk]=createSignal(false); const go = () => { setOk(true); box.forceActiveFocus(); }; return <div><button onClick={() => go()}>go</button><Show when={ok()}><div ref={box} class="b" /></Show></div>; }`, "c.tsx");
  const out = Object.values(app.components)[0] ?? app.entry;
  assert.match(out, /property var _ref_box: null/);                       // root holder
  assert.match(out, /id: _ref_box__lazy\s*Component\.onCompleted: _ref_box = _ref_box__lazy/);
  assert.match(out, /Component\.onDestruction: if \(_ref_box === _ref_box__lazy\) _ref_box = null/);
  assert.match(out, /_ref_box\.forceActiveFocus\(\)/);                   // outside uses read the root prop
  assert.doesNotMatch(out, /id: _ref_box\s*$/m);
});

test("Show: nested Shows gate independently (inner Repeater inside the outer delegate)", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [a,setA]=createSignal(true); const [b,setB]=createSignal(true); return <div><Show when={a()}><div class="o"><Show when={b()}><text>in</text></Show></div></Show></div>; }`);
  assert.match(out, /model: \(a\) \? 1 : 0\s*W\.Div \{\s*cssClass: \["o"\]\s*Repeater \{\s*model: \(b\) \? 1 : 0/);
});

test("For: becomes a Repeater whose delegate binds item to modelData", async () => {
  const out = await qml(`import { For } from "solid-js"; export function C(){ const [items,setItems]=createSignal([]); return <For each={items()}>{(item)=><text>{item}</text>}</For>; }`);
  assert.match(out, /Repeater \{/);
  assert.match(out, /model: items/);
  assert.match(out, /W\.Text \{[\s\S]*text: "" \+ \(modelData\)/);
});

test("For nested in For: the outer item stays reachable inside the inner delegate", async () => {
  // Both delegates bind their row to `modelData`, so the inner one used to SHADOW the outer:
  // `g.name` inside the inner delegate read the inner row instead.
  const out = await qml(`import { For } from "solid-js"; export function C(){ const [groups,setGroups]=createSignal([]); return <For each={groups()}>{(g)=><div><For each={g.items}>{(it)=><text>{g.name}: {it}</text>}</For></div>}</For>; }`);
  // The outer delegate root publishes its row under an id the inner delegate can see.
  const id = out.match(/id: (__for\d+)\n\s*property var __forItem: modelData/);
  assert.ok(id, out);
  assert.match(out, new RegExp(`model: ${id![1]}\\.__forItem\\.items`));
  assert.match(out, new RegExp(`text: "" \\+ \\(${id![1]}\\.__forItem\\.name\\) \\+ ": " \\+ \\(modelData\\)`));
});

test("For nested in For: a delegate root that already has an id (ref) publishes the row under it", async () => {
  const out = await qml(`import { For } from "solid-js"; export function C(){ const [groups,setGroups]=createSignal([]); let box; return <For each={groups()}>{(g)=><div ref={box}><For each={g.items}>{(it)=><text>{g.name}</text>}</For></div>}</For>; }`);
  const ids = out.match(/^\s*id: \S+$/gm) ?? [];
  assert.equal(ids.length, 1, out);   // one object, one id: the ref's
  assert.match(out, /id: _ref_box/);
  assert.match(out, /property var __forItem: modelData/);
  assert.match(out, /text: "" \+ \(_ref_box\.__forItem\.name\)/);
  assert.doesNotMatch(out, /__for\d+/);
});

test("For without a nested loop is emitted exactly as before (no id, no extra property)", async () => {
  const out = await qml(`import { For } from "solid-js"; export function C(){ const [items,setItems]=createSignal([]); return <For each={items()}>{(item)=><div><text>{item}</text></div>}</For>; }`);
  assert.doesNotMatch(out, /__forItem|id: __for/);
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

test("Show: the fallback is mounted only while `when` does NOT hold", async () => {
  const out = await qml(`import { Show } from "solid-js"; export function C(){ const [c,setC]=createSignal(true); return <Show when={c()} fallback={<text>none</text>}><text>yes</text></Show>; }`);
  assert.match(out, /model: \(c\) \? 1 : 0\s*W\.Text \{\s*text: "yes"/);
  assert.match(out, /model: \(!\(c\)\) \? 1 : 0\s*W\.Text \{\s*text: "none"/);
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
  assert.match(out, /W\.Div \{/);
  assert.match(out, /cssPrimitive: "h"/);
  assert.match(out, /cssClass: \["headline"\]/);
});

test("onClick nested in onClick: the outer click area sits beneath the content", async () => {
  // A filling MouseArea is declared after the children, so it lay ON TOP of every descendant and
  // swallowed the inner element's click (a chip inside a clickable list row never fired). On the
  // web the innermost target gets the click; the outer area goes under the content instead.
  const out = await qml(`export function C(){ const [n,setN]=createSignal(0); return <div class="row" onClick={() => setN(1)}><text>t</text><div class="chip" onClick={() => setN(2)}><text>c</text></div></div>; }`);
  const areas = out.split("MouseArea {").slice(1);
  assert.equal(areas.length, 2, out);
  // the row's area is the LAST one emitted (after its children) and is lowered...
  assert.match(areas[1], /^\s*id: __hover\d+\s*\n\s*z: -1/);
  // ...the chip's own area keeps the default stacking.
  assert.doesNotMatch(areas[0].split("}")[0], /z: -1/);
});

test("onClick without an interactive descendant keeps the default stacking", async () => {
  const out = await qml(`export function C(){ const [n,setN]=createSignal(0); return <div class="row" onClick={() => setN(1)}><text>t</text></div>; }`);
  assert.doesNotMatch(out, /z: -1/);
});

test("title: a clickable element's tooltip follows its click area's hover", async () => {
  const out = await qml(`export function C(){ const [n,setN]=createSignal(0); return <div class="tool" title="Arquivar" onClick={() => setN(1)}><text>▣</text></div>; }`);
  const id = out.match(/MouseArea \{\s*\n\s*id: (__hover\d+)/)![1];
  assert.match(out, new RegExp(`W\\.ToolTip \\{\\s*\\n\\s*text: "Arquivar"\\s*\\n\\s*shown: ${id}\\.containsMouse`));
  assert.doesNotMatch(out, /HoverHandler/);
});

test("title: a plain element gets a passive HoverHandler for its tooltip", async () => {
  const out = await qml(`export function C(){ const [t,setT]=createSignal("dica"); return <div class="x" title={t()}><text>a</text></div>; }`);
  const id = out.match(/HoverHandler \{\s*\n\s*id: (__tip\d+)/)![1];
  assert.match(out, new RegExp(`W\\.ToolTip \\{\\s*\\n\\s*text: t\\s*\\n\\s*shown: ${id}\\.hovered`));
});

test("title: an element without one gets no tooltip", async () => {
  const out = await qml(`export function C(){ return <div class="x"><text>a</text></div>; }`);
  assert.doesNotMatch(out, /ToolTip|HoverHandler/);
});
