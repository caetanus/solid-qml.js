import { test } from "node:test";
import assert from "node:assert/strict";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals, analyzeContexts, analyzeProvider } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { emitComponentType } from "../src/emit/component.ts";
import type { Scope } from "../src/emit/expr.ts";
import * as t from "@babel/types";

async function qml(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const render = findRender(ast);
  if (!render) throw new Error("no render");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding" };
  return emitQml(render, scope).join("\n");
}

test("emitQml: div -> W.Div component with cssClass (cssPrimitive default omitted)", async () => {
  const out = await qml(`export function F(){ return <div class="box"></div>; }`);
  assert.match(out, /W\.Div \{/);
  assert.match(out, /cssClass: \["box"\]/);
  assert.doesNotMatch(out, /cssPrimitive: "div"/); // the component defaults it — smaller output
});

test("emitQml: non-div block tag sets cssPrimitive on W.Div", async () => {
  const out = await qml(`export function F(){ return <section class="s"></section>; }`);
  assert.match(out, /W\.Div \{/);
  assert.match(out, /cssPrimitive: "section"/);
});

test("emitQml: literal text child -> W.Text", async () => {
  const out = await qml(`export function F(){ return <div><text>hello</text></div>; }`);
  assert.match(out, /W\.Text \{/);
  assert.match(out, /text: "hello"/);
});

test("emitQml: nested elements nest", async () => {
  const out = await qml(`export function F(){ return <div class="a"><div class="b"></div></div>; }`);
  assert.match(out, /cssClass: \["a"\][\s\S]*cssClass: \["b"\]/);
});

test("emitQml: interpolation child reads the signal as a bound expression", async () => {
  const out = await qml(`export function F(){ const [count,setCount]=createSignal(0); return <text>count: {count()}</text>; }`);
  assert.match(out, /text: "count: " \+ \(count\)/);
});

test("emitQml: button -> W.Button (widget-library component) with text + onClicked", async () => {
  const out = await qml(`export function F(){ const [count,setCount]=createSignal(0); return <button onClick={() => setCount(count() + 1)}>go</button>; }`);
  // One .qml per component: <button> instantiates the library Button, wiring text + the click.
  assert.match(out, /W\.Button \{/);
  assert.match(out, /text: "go"/);
  assert.match(out, /onClicked: count = count \+ 1/);
});

test("emitQml: a component instance is instantiated with prop bindings", async () => {
  const { ast } = await normalize(`export function App(){ return <Greeting name="ada" />; }`, "f.tsx");
  const render = findRender(ast)!;
  const scope: Scope = { table: new Map(), mode: "binding", components: new Map([["Greeting", "Greeting"]]) };
  const out = emitQml(render, scope).join("\n");
  assert.match(out, /Greeting \{/);
  assert.match(out, /name: "ada"/);
});

test("emitQml: an instance emits its children into the default slot", async () => {
  const { ast } = await normalize(`export function App(){ return <Card><text>hi</text></Card>; }`, "f.tsx");
  const render = findRender(ast)!;
  const scope: Scope = { table: new Map(), mode: "binding", components: new Map([["Card", "Card"]]) };
  const out = emitQml(render, scope).join("\n");
  assert.match(out, /Card \{/);
  assert.match(out, /W\.Text \{[\s\S]*text: "hi"/); // child mounts inside the instance
});

test("emitComponentType: {props.children} at the root is dropped (mounted via default property)", async () => {
  const { ast } = await normalize(`export function Card(props){ return <div class="card">{props.children}</div>; }`, "c.tsx");
  const fn = (ast as any).program.body.find((n: any) => n.type === "ExportNamedDeclaration").declaration;
  const render = findRender(ast)!;
  const lines = emitComponentType(fn, render, new Map()).join("\n");
  assert.doesNotMatch(lines, /property var children/);  // not a plain prop
  assert.doesNotMatch(lines, /text: .*children/);        // not emitted as text
  assert.match(lines, /cssClass: \["card"\]/);
});

test("emitComponentType: {props.children} outside the root errors (explicit slot is a later plan)", async () => {
  const { ast } = await normalize(`export function Bad(props){ return <div><span>{props.children}</span></div>; }`, "b.tsx");
  const fn = (ast as any).program.body.find((n: any) => n.type === "ExportNamedDeclaration").declaration;
  const render = findRender(ast)!;
  assert.throws(() => emitComponentType(fn, render, new Map()), /props\.children outside the root/);
});

test("emitQml: ref={x} gives the element an id and resolves x to it", async () => {
  const { ast } = await normalize(`export function R(){ let box; return <div ref={box}><text>hi</text></div>; }`, "r.tsx");
  const render = findRender(ast)!;
  const scope: Scope = { table: new Map(), mode: "binding", components: new Map() };
  const out = emitQml(render, scope).join("\n");
  assert.match(out, /id: _ref_box/);
});

test("emitQml: <Index> emits a Repeater; item() -> modelData, index param -> index", async () => {
  const { ast } = await normalize(
    `export function L(){ return <Index each={xs}>{(item, i) => <text>{i}: {item()}</text>}</Index>; }`, "l.tsx");
  const render = findRender(ast)!;
  const scope: Scope = { table: new Map(), mode: "binding", components: new Map() };
  const out = emitQml(render, scope).join("\n");
  assert.match(out, /Repeater \{/);
  assert.match(out, /model: xs/);
  assert.match(out, /modelData/);  // item() resolved to modelData
  assert.match(out, /index/);      // i resolved to the Repeater index
});

test("emitQml: a button with an element child emits the label + the nested element", async () => {
  const { ast } = await normalize(
    `export function B(){ return <button class="cta" onClick={() => x()}>go<div class="badge">1</div></button>; }`, "b.tsx");
  const render = findRender(ast)!;
  const scope: Scope = { table: new Map(), mode: "binding", components: new Map() };
  const out = emitQml(render, scope).join("\n");
  assert.match(out, /W\.Button \{/);
  assert.match(out, /text: "go"/);                 // text child → label
  assert.match(out, /W\.Div \{[\s\S]*cssClass: \["badge"\]/); // element child → nested W.Div
});

// --- Option 1: same-name prop+signal collapse ---

/** Helper: emit a CounterProvider-like component and return the joined QML lines. */
async function emitProvider(src: string): Promise<string> {
  const { ast } = await normalize(src, "ctx.tsx");
  const file = ast as t.File;
  const contexts = analyzeContexts(file);
  let fn: t.Function | null = null;
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node)) fn = node;
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  }
  if (!fn) throw new Error("no fn");
  const table = analyzeSignals(fn);
  const provider = analyzeProvider(fn, table, contexts);
  let render: t.CallExpression | null = null;
  for (const s of (fn.body as t.BlockStatement).body) {
    if (t.isReturnStatement(s) && s.argument && t.isCallExpression(s.argument)) render = s.argument;
  }
  if (!render) throw new Error("no render");
  return emitComponentType(fn, render, new Map(), contexts, provider).join("\n");
}

test("emitComponentType: same-name prop+signal with || default → emits the default only (Option 1)", async () => {
  const out = await emitProvider(`
    const Ctx = createContext();
    function P(props) {
      const [count, setCount] = createSignal(props.count || 0);
      return <Ctx.Provider value={{ count }}>{props.children}</Ctx.Provider>;
    }
  `);
  // Must NOT produce a self-referential binding
  assert.doesNotMatch(out, /count: count/);
  // Must emit the default (right-hand side) only
  assert.match(out, /property var count: 0/);
});

test("emitComponentType: same-name prop+signal with ?? default → emits the default only (Option 1)", async () => {
  const out = await emitProvider(`
    const Ctx = createContext();
    function P(props) {
      const [val, setVal] = createSignal(props.val ?? 42);
      return <Ctx.Provider value={{ val }}>{props.children}</Ctx.Provider>;
    }
  `);
  assert.doesNotMatch(out, /val: val/);
  assert.match(out, /property var val: 42/);
});

test("emitComponentType: same-name prop+signal with exact prop ref → emits undefined (Option 1)", async () => {
  const out = await emitProvider(`
    const Ctx = createContext();
    function P(props) {
      const [label, setLabel] = createSignal(props.label);
      return <Ctx.Provider value={{ label }}>{props.children}</Ctx.Provider>;
    }
  `);
  assert.match(out, /property var label: undefined/);
});

test("emitComponentType: non-colliding signal init is unchanged (Option 1 does not affect other signals)", async () => {
  const out = await emitProvider(`
    const Ctx = createContext();
    function P(props) {
      const [score, setScore] = createSignal(props.initial || 0);
      return <Ctx.Provider value={{ score }}>{props.children}</Ctx.Provider>;
    }
  `);
  // 'score' != 'initial', so no collapse — emits the full expression
  assert.match(out, /property var score: initial \|\| 0/);
});

test("emitQml: <img class='a' src={u} /> -> W.Image (widget-library component) with src binding", async () => {
  const out = await qml(`export function F(){ const [u,setU]=createSignal(""); return <img class="a" src={u()} />; }`);
  // One .qml per component: <img> instantiates the library Image; CssImage internals live in Image.qml.
  assert.match(out, /W\.Image \{/);
  assert.doesNotMatch(out, /Css\.CssImage/);
  assert.match(out, /cssClass: \["a"\]/);
  assert.match(out, /src: u \|\| ""/);
});

test("emitQml: Image.qml component exists and extends CssImage with a src slot", async () => {
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Image.qml", import.meta.url)), "utf8");
  assert.match(src, /Css\.CssImage \{/);
  assert.match(src, /property url src/);
  assert.match(src, /source: root\.src/);
});

// --- Task 3: <input> ---

async function qmlType(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const file = ast as t.File;
  let fn: t.Function | null = null;
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node)) fn = node;
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  }
  if (!fn) throw new Error("no fn");
  const render = findRender(ast)!;
  return emitComponentType(fn, render, new Map()).join("\n");
}

test("emitQml: <input class='x' value={s()} onInput={...} /> -> CssFill + T.TextField + Binding", async () => {
  const out = await qmlType(`
    export function F() {
      const [s, setS] = createSignal("");
      return <input class="x" value={s()} onInput={(e) => setS(e.currentTarget.value)} />;
    }
  `);
  // Outer wrapper carries the CSS identity and pseudo-class state.
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssClass: \["x"\]/);
  assert.match(out, /cssPrimitive: "input"/);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)/);
  // Implicit size follows the control so CSS width/height still overrides.
  assert.match(out, /implicitWidth: __input0\.implicitWidth/);
  // Native control — chromeless (background null) so our CssFill owns the box visuals.
  assert.match(out, /T\.TextField \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
  assert.match(out, /background: null/);
  // CSS colour/font bridged from the wrapper's inherited properties.
  assert.match(out, /color: cssTheme\.parseColor\(parent\.inheritedColor/);
  assert.match(out, /font\.pixelSize: cssTheme\.parseFontSize/);
  // onTextEdited handler: e.currentTarget.value → text (translated by translateInputHandler).
  assert.match(out, /onTextEdited: \{ s = text \}/);
  // Binding element persists the signal value into the control (survives user edits).
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "text"/);
  assert.match(out, /value: s/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
  // No old-style Connections/onCompleted (replaced by Binding).
  assert.doesNotMatch(out, /Component\.onCompleted/);
  assert.doesNotMatch(out, /TextInput \{/); // bare TextInput is gone; T.TextField is used
});

// --- Task 3: classList={{ cls: expr }} ---

test("emitQml: classList={{ completed: done() }} with signal -> reactive cssClass concat", async () => {
  const out = await qml(`
    export function F() {
      const [done, setDone] = createSignal(false);
      return <div class="row" classList={{ completed: done() }}>hi</div>;
    }
  `);
  // Reactive concat form — condition is bare (binding mode, QML tracks dep on `done`)
  assert.match(out, /cssClass: \["row"\]\.concat\(done \? \["completed"\] : \[\]\)/);
  // Must NOT use __self. inside the concat (that would break QML binding tracking)
  assert.doesNotMatch(out, /__self\.done/);
});

test("emitQml: classList with multiple entries chains multiple .concat()", async () => {
  const out = await qml(`
    export function F() {
      const [active, setActive] = createSignal(false);
      const [disabled, setDisabled] = createSignal(false);
      return <div class="btn" classList={{ active: active(), disabled: disabled() }}>x</div>;
    }
  `);
  // Two concat chains, one per classList entry
  assert.match(out, /cssClass: \["btn"\]\.concat\(active \? \["active"\] : \[\]\)\.concat\(disabled \? \["disabled"\] : \[\]\)/);
});

test("emitQml: div without classList keeps the original static cssClass form (goldens unchanged)", async () => {
  const out = await qml(`export function F(){ return <div class="box"></div>; }`);
  // Must be the original static form, not a concat expression
  assert.match(out, /cssClass: \["box"\]/);
  assert.doesNotMatch(out, /\.concat\(/);
});

test("emitQml: classList without a static class uses empty base array", async () => {
  const out = await qml(`
    export function F() {
      const [active, setActive] = createSignal(false);
      return <div classList={{ active: active() }}>x</div>;
    }
  `);
  // Base array is empty [], conditional appended
  assert.match(out, /cssClass: \[\]\.concat\(active \? \["active"\] : \[\]\)/);
});

test("emitQml: <></> fragment children emit inline into the parent (no node of its own)", async () => {
  const out = await qml(`
    export function F() {
      return <div class="host"><><text>a</text><text>b</text></></div>;
    }
  `);
  // Both texts, directly under the host — exactly one W.Div (the host), no wrapper box
  assert.match(out, /text: "a"[\s\S]*text: "b"/);
  assert.equal(out.match(/W\.Div \{/g)?.length, 1);
});

test("emitQml: fragment under Show inherits the guard on each child", async () => {
  const out = await qml(`
    export function F() {
      const [flag, setFlag] = createSignal(false);
      return <div><Show when={flag()}><><text>x</text><text>y</text></></Show></div>;
    }
  `);
  assert.equal(out.match(/visible: !!\(flag\)/g)?.length, 2);
});

test("emitComponentType: root <></> fragment wraps in a primitive-less box hosting the children", async () => {
  const { ast } = await normalize(
    `export function F(){ return <><text>a</text><text>b</text></>; }`, "f.tsx");
  const fn = (ast as any).program.body.find((n: any) => n.type === "ExportNamedDeclaration").declaration;
  const render = findRender(ast)!;
  const lines = emitComponentType(fn, render, new Map()).join("\n");
  assert.match(lines, /W\.Div \{/);                // the wrapper host (root fragment → W.Div)
  assert.match(lines, /text: "a"[\s\S]*text: "b"/);
  assert.doesNotMatch(lines, /cssClass:/);          // wrapper carries no class
});
