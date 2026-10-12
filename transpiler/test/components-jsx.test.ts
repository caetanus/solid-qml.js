import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

// Release-plan M2 ("no silent miscompiles"), item 2: component-level JSX that compiled but was wrong
// at runtime — <Dynamic component={Comp}> (emitted a text element), spread props on a component
// (dropped), the children() helper (miscompiled), block-bodied onClick (rejected) — now works like
// Solid or fails loud.

const HEAD = `import { createSignal, mergeProps, splitProps, children } from "solid-js";
import { Dynamic } from "solid-js/web";
`;
async function gen(body: string) {
  return generate(HEAD + body, "/virtual/components.tsx");
}
const rejects = (body: string, msg: RegExp) => assert.rejects(gen(body), msg);

// ── block-bodied handlers ───────────────────────────────────────────────────────────────────────
test("onClick={() => { … }}: the block runs as its own function (a `return` ends only the handler)", async () => {
  const app = await gen(`export function App() {
    const [n, setN] = createSignal(0);
    return <div><button onClick={() => { if (n() > 2) return; setN(n() + 1); setN(n() * 2); }}>{n()}</button>
      <div role="button" onClick={() => { setN(0); }}>reset</div></div>;
  }`);
  assert.match(app.entry, /onClicked: \(function\(\) \{ if \(n > 2\) \{ return undefined; \} n = n \+ 1; n = n \* 2; \}\)\(\)/);
  // Keyboard activation runs the same function and still accepts the key afterwards.
  assert.match(app.entry, /if \(event\.key === Qt\.Key_Return \|\| event\.key === Qt\.Key_Enter\) \{ \(function\(\) \{ n = 0; \}\)\(\); event\.accepted = true; \}/);
});

// ── spread props ────────────────────────────────────────────────────────────────────────────────
test("<C {...o} />: every prop C declares reads the spread object", async () => {
  const app = await gen(`function C(p) { return <text>{p.a} {p.b}</text>; }
    export function App() { const o = { a: 1, b: "x" }; return <div><C {...o} /></div>; }`);
  assert.match(app.entry, /C \{\n\s+a: \(o\)\.a\n\s+b: \(o\)\.b\n/);
});

test("<C x={1} {...o} y={2}>: last source with a defined value wins (Solid's mergeProps rule)", async () => {
  const app = await gen(`function C(p) { return <text>{p.x} {p.y} {p.z}</text>; }
    export function App() { const [o] = createSignal({ x: 5 }); return <div><C x={1} {...o()} y={2} /></div>; }`);
  // (x/y/z are QML-reserved-ish names: safeName appends `_`.)
  assert.match(app.entry, /x_: \(function\(\) \{ var __v; __v = \(o\)\.x; if \(__v !== undefined\) return __v; return 1; \}\)\(\)/);
  assert.match(app.entry, /y_: 2\n/, "an explicit literal after the spread is final");
  assert.match(app.entry, /z_: \(o\)\.z\n/);
});

test("{...props} forwards: the forwarding component declares the inner component's props (two levels)", async () => {
  const app = await gen(`function Inner(p) { return <text>{p.label}:{p.count}</text>; }
    function Mid(p) { return <div><Inner {...p} /></div>; }
    function Outer(p) { const m = mergeProps({ count: 3 }, p); return <section><Mid {...m} /></section>; }
    export function App() { return <div><Outer label="hi" /></div>; }`);
  assert.match(app.components.Mid, /property var label\n\s+property var count\n/);
  assert.match(app.components.Mid, /Inner \{\n\s+label: label\n\s+count: count\n/);
  assert.match(app.components.Outer, /property var count: 3/);
  assert.match(app.components.Outer, /Mid \{\n\s+label: label\n\s+count: count\n/);
  assert.match(app.entry, /Outer \{\n\s+label: "hi"\n/);
});

test("a component whose root is another component: a forwarded prop is that component's own property", async () => {
  const app = await gen(`function Inner(p) { return <text>{p.label}:{p.count}</text>; }
    function Wrap(p) { const m = mergeProps({ count: 3 }, p); return <Inner {...m} />; }
    function Fwd(p) { return <Inner label={p.label} count={1} />; }
    export function App() { return <div><Wrap label="a" /><Fwd label="b" /></div>; }`);
  // No `property var label` re-declared on the Inner-typed root, no `label: label` self-binding.
  assert.equal(app.components.Wrap.split("\n").filter((l) => /^Inner \{|^\s+(id|count|label)/.test(l)).join("|"), "Inner {|    id: __self|    count: 3");
  assert.equal(app.components.Fwd.split("\n").filter((l) => /^Inner \{|^\s+(id|count|label)/.test(l)).join("|"), "Inner {|    id: __self|    count: 1");
  await rejects(`function Inner(p) { return <text>{p.label}</text>; } function Bad(p) { return <Inner label={p.label + "!"} />; }
    export function App() { return <Bad label="x" />; }`, /<Inner> is this component's root and already has a prop `label`/);
});

test("splitProps rest: spread without the split-off keys; others.x reads the prop", async () => {
  const app = await gen(`function Inner(p) { return <text>{p.a}{p.b}</text>; }
    function W(p) { const [local, others] = splitProps(p, ["a"]); return <div><text>{local.a}{others.b}</text><Inner {...others} /></div>; }
    export function App() { return <W a={1} b={2} />; }`);
  assert.match(app.components.W, /Inner \{\n\s+b: b\n\s+\}/);
  assert.doesNotMatch(app.components.W, /Inner \{\n\s+a:/);
});

test("fail loud: spread on a foreign .qml type, children forwarded only through a spread", async () => {
  await assert.rejects(generate(`import Fancy from "./Fancy.qml"; export function App() { const o = { a: 1 }; return <Fancy {...o} />; }`,
    "/virtual/f.tsx", { readFile: async () => "import QtQuick\nItem {}\n" }), /spread props on a foreign QML type are not supported/);
  await rejects(`function Inner(p) { return <div>{p.children}</div>; } function Mid(p) { return <Inner {...p} />; }
    export function App() { return <Mid><text>x</text></Mid>; }`, /forwarding children through a props spread is not supported natively — pass them explicitly/);
});

// ── children ────────────────────────────────────────────────────────────────────────────────────
test("children(() => props.children): {c()} in the root slots the children", async () => {
  const app = await gen(`function Card(p) { const c = children(() => p.children); return <div class="card">{c()}</div>; }
    export function App() { return <div><Card><text>x</text></Card></div>; }`);
  assert.match(app.components.Card, /W\.Div \{\n\s+id: __self\n\s+cssClass: \["card"\]\n\}/);
  assert.match(app.entry, /Card \{\n\s+W\.Text \{\n\s+text: "x"/);
});

test("a mergeProps alias's .children slots too (it rendered QML's own `children` list as text)", async () => {
  const app = await gen(`function Card(p) { const m = mergeProps({ t: "a" }, p); return <div>{m.t}{m.children}</div>; }
    export function App() { return <Card><text>x</text></Card>; }`);
  assert.doesNotMatch(app.components.Card, /\(children\)/);
});

test("children a component never reads are not rendered (Solid never evaluates them)", async () => {
  const app = await gen(`function Plain(p) { return <text>{p.n}</text>; } export function App() { return <Plain n={1}><text>hidden</text></Plain>; }`);
  assert.doesNotMatch(app.entry, /hidden/);
});

test("fail loud: inspecting children", async () => {
  await rejects(`function Card(p) { const c = children(() => p.children); return <div>{c.toArray().length}</div>; }
    export function App() { return <Card><text>x</text></Card>; }`, /children\(\): `c` can only be rendered as \{c\(\)\} directly inside the component's root element/);
  await rejects(`function Card(p) { const c = children(() => p.children.length); return <div>{c()}</div>; }
    export function App() { return <Card><text>x</text></Card>; }`, /only `const c = children\(\(\) => props\.children\)` is supported natively/);
});

// ── <Dynamic> ───────────────────────────────────────────────────────────────────────────────────
test("<Dynamic component={cond ? A : B} …props>: one lazily mounted branch per candidate", async () => {
  const app = await gen(`function A(p) { return <text>A{p.n}</text>; } function B(p) { return <div>B{p.n}{p.children}</div>; }
    export function App() { const [s, setS] = createSignal(0); return <div><Dynamic component={s() > 0 ? A : B} n={s()}><text>kid</text></Dynamic></div>; }`);
  assert.match(app.entry, /Repeater \{\n\s+model: \(\(s > 0 \? "c:A" : "c:B"\) === "c:A"\) \? 1 : 0\n\s+A \{\n\s+n: s\n\s+\}/);
  assert.match(app.entry, /model: \(\(s > 0 \? "c:A" : "c:B"\) === "c:B"\) \? 1 : 0\n\s+B \{\n\s+n: s\n\s+W\.Text \{\n\s+text: "kid"/);
  assert.ok(app.components.A && app.components.B, "candidates are followed and emitted");
});

test("<Dynamic component={ok && A}> renders nothing while falsy; tag strings mix with components", async () => {
  const app = await gen(`function A() { return <text>A</text>; }
    export function App() { const [s] = createSignal(0); return <div><Dynamic component={s() === 1 && A} /><Dynamic component={s() ? A : "section"}><text>t</text></Dynamic></div>; }`);
  assert.match(app.entry, /model: \(\(s === 1 \? "c:A" : ""\) === "c:A"\) \? 1 : 0/);
  assert.match(app.entry, /model: \(\(s \? "c:A" : "t:section"\) === "t:section"\) \? 1 : 0\n\s+W\.Div \{\n\s+cssPrimitive: "section"/);
});

test("<Dynamic component={tag()}> with element children: a generic element (they were text-bound as h() calls)", async () => {
  const app = await gen(`export function App() { const [tag] = createSignal("section"); return <div><Dynamic component={tag()}><text>x</text></Dynamic></div>; }`);
  assert.match(app.entry, /W\.Div \{\n\s+cssPrimitive: tag\n\s+W\.Text \{\n\s+text: "x"/);
});

test("fail loud: a component held in a runtime value", async () => {
  await rejects(`function A() { return <text>A</text>; } function B() { return <text>B</text>; }
    export function App() { const [c] = createSignal(A); return <div><Dynamic component={c()} /><B /></div>; }`,
    /<Dynamic component=\{…\}>: the component must be visible at compile time/);
});

// ── props shapes that were silently wrong ───────────────────────────────────────────────────────
test("fail loud: mergeProps override form and a destructured props parameter", async () => {
  await rejects(`function C(p) { const m = mergeProps(p, { a: 1 }); return <text>{m.a}</text>; } export function App() { return <C a={2} />; }`,
    /mergeProps\(props, \{ … \}\): an object after props OVERRIDES the passed props — not supported natively; put the defaults first/);
  await rejects(`function C({ a }) { return <text>{a}</text>; } export function App() { return <C a={2} />; }`,
    /a component's props must be one parameter .* destructuring it .* not supported natively/);
});
