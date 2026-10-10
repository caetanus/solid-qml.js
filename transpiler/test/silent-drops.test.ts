// The transpiler must never SILENTLY drop what it cannot emit (roadmap "silent drop" family):
// a multi-root control-flow body, a handler on a text, a dynamic attribute. Each is either emitted
// faithfully or reported at transpile time.
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

const HEAD = `import { createSignal, Show, For, Index, Switch, Match } from "solid-js";`;
const wrap = (body: string) =>
  `${HEAD} export function C(){ const [a,setA]=createSignal(true); const [b,setB]=createSignal(true); const [xs,setXs]=createSignal([]); return <div>${body}</div>; }`;

/** Every object nested in a single-root slot — a `Repeater {` (its default property is `delegate`)
 *  or a `Component {` — as [type, number of OBJECT children]. A slot with 2+ object children keeps
 *  only the last one at runtime: that is the silent drop. */
function singleRootSlots(out: string): Array<[string, number]> {
  const lines = out.split("\n");
  const res: Array<[string, number]> = [];
  for (let i = 0; i < lines.length; i++) {
    const m = /^(\s*)(?:\w+: )?(Repeater|Component) \{\s*$/.exec(lines[i]);
    if (!m) continue;
    const inner = m[1] + "    ";
    let kids = 0;
    for (let j = i + 1; j < lines.length; j++) {
      if (lines[j].startsWith(`${m[1]}}`)) break;
      if (lines[j].startsWith(inner) && !lines[j].startsWith(inner + " ") && /^[\w.]+ \{\s*$/.test(lines[j].slice(inner.length))) kids++;
    }
    res.push([m[2], kids]);
  }
  return res;
}

function assertSingleRoots(out: string) {
  for (const [type, kids] of singleRootSlots(out)) assert.equal(kids, 1, `${type} with ${kids} roots:\n${out}`);
}

test("multi-child <Show> nested in a <Show>: every child keeps its own gate (a fragment, not one delegate)", async () => {
  const out = await qml(wrap(`<Show when={a()}><text>one</text><Show when={b()}><div class="panel"/><div class="panel2"/></Show></Show>`));
  assertSingleRoots(out);
  assert.match(out, /text: "one"/);
  assert.match(out, /cssClass: \["panel"\]/);
  assert.match(out, /cssClass: \["panel2"\]/);
  // Both inner children are still gated by BOTH conditions (outer a, inner b).
  assert.equal(out.match(/model: \(a\) \? 1 : 0/g)?.length, 3, out);
  assert.equal(out.match(/model: \(b\) \? 1 : 0/g)?.length, 2, out);
  // No implicit wrapper box: the delegates are the elements themselves.
  assert.equal(out.match(/W\.Div \{/g)?.length, 3, out); // root + panel + panel2
});

test("a <Show> with a fallback nested in a <Show>: branch and fallback both survive", async () => {
  const out = await qml(wrap(`<Show when={a()}><Show when={b()} fallback={<text>f</text>}><text>y</text></Show></Show>`));
  assertSingleRoots(out);
  assert.match(out, /model: \(b\) \? 1 : 0\s*W\.Text \{\s*text: "y"/);
  assert.match(out, /model: \(!\(b\)\) \? 1 : 0\s*W\.Text \{\s*text: "f"/);
});

test("a single nested <Show> is emitted exactly as before (one outer gate around the inner one)", async () => {
  const out = await qml(wrap(`<Show when={a()}><Show when={b()}><text>x</text></Show></Show>`));
  assert.match(out, /Repeater \{\n\s*model: \(a\) \? 1 : 0\n\s*Repeater \{\n\s*model: \(b\) \? 1 : 0\n\s*W\.Text/);
  assert.equal(out.match(/Repeater \{/g)?.length, 2);
});

test("<Match> whose body is a multi-root <Show>: one incubator per root, same guard", async () => {
  const out = await qml(wrap(`<Switch><Match when={a()}><Show when={b()} fallback={<text>f</text>}><text>y</text></Show></Match></Switch>`));
  assertSingleRoots(out);
  assert.equal(out.match(/Css\.CssIncubator \{/g)?.length, 2, out);
  assert.match(out, /text: "y"/);
  assert.match(out, /text: "f"/);
});

test("<For>/<Index> row with several roots: a clear transpile error, never a dropped child", async () => {
  await assert.rejects(qml(wrap(`<For each={xs()}>{(x) => <><text>a</text><text>b</text></>}</For>`)), /<For> row .*single root/);
  await assert.rejects(qml(wrap(`<For each={xs()}>{(x) => <Show when={x.ok} fallback={<text>n</text>}><text>y</text></Show>}</For>`)), /<For> row .*single root/);
  await assert.rejects(qml(wrap(`<Index each={xs()}>{(x) => <><text>a</text><text>b</text></>}</Index>`)), /<Index> row .*single root/);
});

test("a single-root <For> row with a lazy <Show> root is still accepted", async () => {
  const out = await qml(wrap(`<For each={xs()}>{(x) => <div><Show when={x.ok}><text>y</text></Show></div>}</For>`));
  assertSingleRoots(out);
});

test("<text onClick>: a hover-tracked click area like the div's (it used to emit a bare W.Text)", async () => {
  const out = await qml(`export function C(){ const name = "n"; const [k,setK]=createSignal(0); const go = () => setK(1); return <div><text class="x" onClick={() => go()}>{name}</text></div>; }`);
  const text = out.slice(out.indexOf("W.Text {"));
  assert.match(text, /^W\.Text \{\n\s*cssClass: \["x"\]\n\s*cssState: (__hover\d+)\.containsMouse \? \["hover"\] : \[\]/);
  assert.match(text, /text: [^\n]+\n\s*MouseArea \{\n\s*id: __hover\d+\n\s*anchors\.fill: parent\n\s*hoverEnabled: true\n\s*cursorShape: Qt\.PointingHandCursor\n\s*onClicked: .+/);
});

test("<text> takes the div's other pointer props too: onContextMenu, title, ref", async () => {
  const out = await qml(`export function C(){ let m; let t; return <div><text ref={t} title="tip" onContextMenu={(e) => m.open(e.clientX, e.clientY)}>x</text></div>; }`);
  const text = out.slice(out.indexOf("W.Text {"));
  assert.match(text, /id: _ref_t/);
  assert.match(text, /acceptedButtons: Qt\.RightButton/);
  assert.match(text, /W\.ToolTip \{\n\s*text: "tip"/);
});

test("<text onClick> and <div onClick> emit the same click area", async () => {
  const d = await qml(`export function C(){ const [k,setK]=createSignal(0); return <div onClick={(e) => { if (e.shiftKey) setK(1); }}>x</div>; }`);
  const tx = await qml(`export function C(){ const [k,setK]=createSignal(0); return <text onClick={(e) => { if (e.shiftKey) setK(1); }}>x</text>; }`);
  const area = (s: string) => s.slice(s.indexOf("MouseArea {"), s.indexOf("}", s.indexOf("onClicked")));
  assert.equal(area(tx), area(d));
});

test("<input placeholder={expr}>: a binding (a dynamic placeholder used to be dropped)", async () => {
  const out = await qml(`export function C(){ const [mode,setMode]=createSignal("a"); return <div><input placeholder={mode() === "a" ? "Search artists" : "Search albums"} /><textarea placeholder={mode()} /></div>; }`);
  assert.match(out, /W\.TextField \{[\s\S]*placeholder: \(?mode === "a"\)? \? "Search artists" : "Search albums"/);
  assert.match(out, /W\.TextArea \{[\s\S]*placeholder: mode\b/);
});

test("<input placeholder=\"…\">: a static placeholder is emitted exactly as before", async () => {
  const out = await qml(`export function C(){ return <div><input placeholder="Find" /><input placeholder="" /></div>; }`);
  assert.equal(out.match(/placeholder: "Find"/g)?.length, 1);
  assert.equal(out.match(/placeholder:/g)?.length, 1);
});

test("disabled={expr} / readOnly={expr}: a binding, not a constant (it used to be disabled forever)", async () => {
  const out = await qml(`export function C(){ const [busy,setBusy]=createSignal(false); return <div><input disabled={busy()} readOnly={busy()} /><textarea disabled={busy()} /><select disabled={busy()} value="a" onChange={() => 0}><option value="a">A</option></select><input type="checkbox" disabled={busy()} /><input type="range" disabled={busy()} /><Dial disabled={busy()} /></div>; }`);
  assert.doesNotMatch(out, /enabled: false/);
  assert.doesNotMatch(out, /disabled: true/);
  assert.equal(out.match(/enabled: !\(busy\)/g)?.length, 5, out);
  assert.match(out, /readOnly: busy/);
  assert.match(out, /W\.Dial \{[\s\S]*disabled: busy/);
});

test("bare / literal disabled and readOnly are emitted exactly as before", async () => {
  const out = await qml(`export function C(){ return <div><input disabled readOnly /><input disabled={false} /><Dial disabled /></div>; }`);
  assert.equal(out.match(/enabled: false/g)?.length, 1, out);
  assert.match(out, /readOnly: true/);
  assert.match(out, /disabled: true/);
});

test("<Switch fallback>: mounted (lazily) only while no <Match> holds — it used to be dropped", async () => {
  const out = await qml(`import { Switch, Match } from "solid-js"; export function C(){ const [n,setN]=createSignal(0); return <div><Switch fallback={<text>many</text>}><Match when={n() === 0}><text>zero</text></Match><Match when={n() === 1}><text>one</text></Match></Switch></div>; }`);
  assert.equal(out.match(/Css\.CssIncubator \{/g)?.length, 3, out);
  assert.match(out, /active: \(!\(\(n === 0\) \|\| \(n === 1\)\)\) \? true : false\n\s*sourceComponent: Component \{\n\s*W\.Text \{\n\s*text: "many"/);
});

test("<Show> with a bare text child or fallback: a transpile error, not a dropped text", async () => {
  await assert.rejects(qml(wrap(`<Show when={a()}>{"loose"}</Show>`)), /wrap it in <text>/);
  await assert.rejects(qml(wrap(`<Show when={a()} fallback={"loading"}><text>x</text></Show>`)), /fallback .*wrap (it|text) in <text>/);
  await assert.rejects(qml(wrap(`<Switch fallback={"none"}><Match when={a()}><text>x</text></Match></Switch>`)), /<Switch>? ?fallback/);
});

test("class={expr}: a dynamic class string binds cssClass (it used to be dropped)", async () => {
  const out = await qml(`export function C(){ const [hot,setHot]=createSignal(false); return <div class={hot() ? "a b" : "c"}><text class={"t " + (hot() ? "x" : "")} classList={{ hot: hot() }}>x</text></div>; }`);
  assert.match(out, /W\.Div \{\n\s*cssClass: \(""\s*\+\s*\(\(hot \? "a b" : "c"\)\s*\?\?\s*""\)\)\.split\(\/\\s\+\/\)\.filter\(Boolean\)/);
  assert.match(out, /W\.Text \{\n\s*cssClass: \(.*\)\.split\(\/\\s\+\/\)\.filter\(Boolean\)\.concat\(hot \? \["hot"\] : \[\]\)/);
});
