import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

// Release-plan M2 ("no silent miscompiles"), item 1: Solid's reactivity API beyond signals/memos/
// effects either works like Solid (untrack, on, batch, effect cleanups, prev values,
// createRenderEffect/createComputed, createSelector) or fails at compile time with a message naming
// the construct (createRoot, createMutable, a primitive called outside a component's top level).
// These were all accepted before and emitted as calls to undefined functions, or dropped.
// Runtime behaviour (cleanup order, untrack not re-running, on() deps, batch coalescing) is probed
// against the real engine — see the commit message for the probe output.

const HEAD = `import { createSignal, createMemo, createEffect, createRenderEffect, createComputed, createSelector,
  createRoot, untrack, batch, on, onMount, onCleanup, For, Show, createContext, useContext } from "solid-js";
import { createMutable } from "solid-js/store";
`;
async function gen(body: string) {
  return generate(HEAD + body, "/virtual/reactivity.tsx");
}
async function qmlOf(body: string): Promise<string> {
  const app = await gen(body);
  return [app.entry, ...Object.values(app.components)].join("\n");
}
/** Every (dependency → computation) re-run wired by a Connections handler, e.g. "__self.A→__effect0". */
const handlers = (qml: string) => [...qml.matchAll(/target: (\S+)\s*function on(\w+)Changed\(\) \{ ((?:__effect\d+\(\); )+)\}/g)]
  .flatMap((m) => m[3].trim().split(" ").map((r) => `${m[1]}.${m[2]}→${r.replace("();", "")}`));

// ── untrack ───────────────────────────────────────────────────────────────────────────────────
test("untrack in a memo / render binding: read through __untrack (a signal handler — no capture)", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2);
    const sum = createMemo(() => a() + untrack(b));
    return <text>{sum()} {untrack(() => a() * 10)}</text>;
  }`);
  assert.match(qml, /readonly property var sum: a \+ __untrack\(function\(\) \{ return b \}\)/);
  assert.match(qml, /text: "" \+ \(sum\) \+ " " \+ \(__untrack\(function\(\) \{ return a \* 10 \}\)\)/);
  assert.match(qml, /function __untrack\(f\) \{ var r = \{\}; __untracker\.run\(function\(\) \{ r\.v = f\(\); \}\); return r\.v; \}/);
  assert.match(qml, /QtObject \{ id: __untracker; signal run\(var f\); onRun: \(f\) => f\(\) \}/);
});

test("untrack in an effect: its reads are not dependencies", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2);
    createEffect(() => console.log(a(), untrack(() => b())));
    return <text>{a()}</text>;
  }`);
  assert.deepEqual(handlers(qml), ["__self.A→__effect0"]);
});

test("no untrack in the component: no __untrack helper emitted", async () => {
  const qml = await qmlOf(`export function App() { const [a] = createSignal(1); return <text>{a()}</text>; }`);
  assert.doesNotMatch(qml, /__untrack/);
});

// ── on(deps, fn, { defer }) ─────────────────────────────────────────────────────────────────────
test("createEffect(on(dep, fn)): only the dep is tracked; fn gets (input, prevInput, prevValue)", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2);
    createEffect(on(a, (v, pv, prev) => { console.log(v, pv, b()); return v; }));
    return <text>{a()}</text>;
  }`);
  assert.deepEqual(handlers(qml), ["__self.A→__effect0"], "b is read in fn, untracked");
  assert.match(qml, /property var __effect0_prev: undefined/);
  assert.match(qml, /property var __effect0_input\n/);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; var __in = a; __effect0_prev = \(function\(v, pv, prev\) \{ console\.log\(v, pv, b\); return v; \}\)\(__in, __effect0_input, __effect0_prev\); __effect0_input = __in; \}/);
});

test("createEffect(on([a, b], fn, { defer: true })): array deps, first run skipped", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2); const [c, setC] = createSignal(3);
    createEffect(on([a, b], ([x, y]) => console.log(x + y + c()), { defer: true }));
    return <text>{a()}</text>;
  }`);
  assert.deepEqual(handlers(qml).sort(), ["__self.A→__effect0", "__self.B→__effect0"]);
  assert.match(qml, /property bool __effect0_deferred: true/);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; var __in = \[a, b\]; if \(__effect0_deferred\) \{ __effect0_deferred = false; return; \} \(function\(\[x_, y_\]\) \{ return console\.log\(x_ \+ y_ \+ c\) \}\)\(__in\); \}/);
});

test("createMemo(on(dep, fn)): deps tracked, fn untracked, memo state kept in a plain object", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2);
    const m = createMemo(on(a, (v) => v + b()));
    return <text>{m()}</text>;
  }`);
  assert.match(qml, /readonly property var __m_state: \(\{ prev: undefined, input: undefined, deferred: false \}\)/);
  assert.match(qml, /readonly property var m: \(function\(\) \{ var __s = __m_state; var __in = a; if \(__s\.deferred\) \{ __s\.deferred = false; return __s\.prev; \} __s\.prev = __untrack\(function\(\) \{ return \(function\(v\) \{ return v \+ b \}\)\(__in, __s\.input, __s\.prev\); \}\); __s\.input = __in; return __s\.prev; \}\)\(\)/);
});

test("createMemo((prev) => …, init): the previous value is passed (was an undefined name)", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1);
    const total = createMemo((prev) => prev + a(), 100);
    return <text>{total()}</text>;
  }`);
  assert.match(qml, /readonly property var __total_state: \(\{ prev: 100 \}\)/);
  assert.match(qml, /readonly property var total: \(function\(\) \{ var __s = __total_state; __s\.prev = \(function\(prev\) \{ return prev \+ a \}\)\(__s\.prev\); return __s\.prev; \}\)\(\)/);
});

test("createMemo reads props/helpers like the render does (was emitted as an undefined `props.x`)", async () => {
  const qml = await qmlOf(`function C(props) { const d = createMemo(() => props.x * 2); return <text>{d()}</text>; }
    export function App() { return <C x={2} />; }`);
  assert.match(qml, /readonly property var d: x_ \* 2/); // the prop `x` (safe-named), not `props.x`
});

// ── effect cleanups and values ──────────────────────────────────────────────────────────────────
test("onCleanup inside an effect: its own list, run (newest first) before each re-run and on unmount", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1);
    onCleanup(() => console.log("component"));
    createEffect(() => { const v = a(); onCleanup(() => console.log("effect", v)); });
    return <text>{a()}</text>;
  }`);
  assert.match(qml, /property var __effect0_cleanups: \[\]/);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; var __c = __effect0_cleanups; __effect0_cleanups = \[\]; for \(var __i = __c\.length - 1; __i >= 0; __i--\) __c\[__i\]\(\); var v = a; __effect0_cleanups\.push\(function\(\) \{ return console\.log\("effect", v\) \}\); \}/);
  assert.match(qml, /Component\.onCompleted: \{ __cleanups\.push\(function\(\) \{ return console\.log\("component"\) \}\); __started\.__effect0 = true; __effect0\(\); \}/);
  assert.match(qml, /Component\.onDestruction: \{ for \(var __i = __effect0_cleanups\.length - 1; __i >= 0; __i--\) __effect0_cleanups\[__i\]\(\); for \(var i = __cleanups\.length - 1; i >= 0; i--\) __cleanups\[i\]\(\); \}/);
});

test("an effect's return value is its next run's prev (Solid semantics, not a cleanup)", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1);
    createEffect((prev) => { console.log(prev, a()); return a(); }, 0);
    return <text>{a()}</text>;
  }`);
  assert.match(qml, /property var __effect0_prev: 0/);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; __effect0_prev = \(function\(prev\) \{ console\.log\(prev, a\); return a; \}\)\(__effect0_prev\); \}/);
});

test("an early return in an effect only ends that effect (it used to end the whole onCompleted)", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1);
    createEffect(() => { if (a() > 1) return; console.log("small"); });
    onMount(() => console.log("mounted"));
    return <text>{a()}</text>;
  }`);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; if \(a > 1\) \{ return undefined; \} console\.log\("small"\); \}/);
  assert.match(qml, /Component\.onCompleted: \{ __started\.__effect0 = true; __effect0\(\); console\.log\("mounted"\); \}/);
});

test("createEffect and onMount run in source order; createRenderEffect/createComputed at their position", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(0);
    console.log("setup 1");
    createEffect(() => console.log("effect", a()));
    onMount(() => console.log("mount"));
    createComputed(() => setB(a() * 2));
    createRenderEffect(() => console.log("render", b()));
    console.log("setup 2");
    return <text>{a()} {b()}</text>;
  }`);
  assert.match(qml, /Component\.onCompleted: \{ console\.log\("setup 1"\); __started\.__effect1 = true; __effect1\(\); __started\.__effect2 = true; __effect2\(\); console\.log\("setup 2"\); __started\.__effect0 = true; __effect0\(\); console\.log\("mount"\); \}/);
  assert.deepEqual(handlers(qml).sort(), ["__self.A→__effect0", "__self.A→__effect1", "__self.B→__effect2"]);
  // One Connections per dependency, its computations in creation order (QML notifies the newest
  // endpoint first, so separate Connections would run them in reverse).
  assert.match(qml, /function onAChanged\(\) \{ __effect0\(\); __effect1\(\); \}/);
  assert.match(qml, /readonly property var __started: \(\{\}\)/);
});

test("createEffect(namedFunction) runs the component function with prev", async () => {
  const qml = await qmlOf(`export function App() {
    const [a, setA] = createSignal(1);
    function sync() { console.log(a()); }
    createEffect(sync);
    return <text>{a()}</text>;
  }`);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; __effect0_prev = \(function\(__p\) \{ return sync\(__p\) \}\)\(__effect0_prev\); \}/);
  assert.deepEqual(handlers(qml), ["__self.A→__effect0"], "the helper's reads are followed");
});

test("an effect reading a context member resolves it through the provider (was an undefined name)", async () => {
  const qml = await qmlOf(`const Ctx = createContext();
    function Prov(p) { const [n, setN] = createSignal(1); return <Ctx.Provider value={{ n }}>{p.children}</Ctx.Provider>; }
    function Child() { const c = useContext(Ctx); createEffect(() => console.log(c.n())); return <text>{c.n()}</text>; }
    export function App() { return <Prov><Child /></Prov>; }`);
  assert.match(qml, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; console\.log\(__ctx_Ctx\.n\); \}/);
});

// ── batch ───────────────────────────────────────────────────────────────────────────────────────
test("batch(fn): computations defer while a batch is open (shared runtime module), fn's value returned", async () => {
  const app = await gen(`export function App() {
    const [a, setA] = createSignal(1); const [b, setB] = createSignal(2);
    createEffect(() => console.log(a() + b()));
    return <button onClick={() => batch(() => { setA(10); setB(20); })}>{a()}</button>;
  }`);
  assert.match(app.entry, /^import "modules\/solid-batch\.mjs" as SolidBatch$/m);
  assert.match(app.entry, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; if \(!SolidBatch\.defer\(__self, "__effect0_run"\)\) __effect0_run\(\); \}/);
  assert.match(app.entry, /function __effect0_run\(\) \{ console\.log\(a \+ b\); \}/);
  assert.match(app.entry, /onClicked: SolidBatch\.batch\(function\(\) \{ a = 10; b = 20; \}\)/);
  assert.match(app.modules["modules/solid-batch.mjs"], /export function batch\(fn\)/);
});

test("batch in one component makes every component's effects batch-aware", async () => {
  const app = await gen(`function Watch(p) { createEffect(() => console.log(p.v)); return <text>{p.v}</text>; }
    export function App() {
      const [a, setA] = createSignal(1);
      return <div><Watch v={a()} /><button onClick={() => batch(() => setA(2))}>x</button></div>;
    }`);
  assert.match(app.components.Watch, /^import "modules\/solid-batch\.mjs" as SolidBatch$/m);
  assert.match(app.components.Watch, /SolidBatch\.defer\(__self, "__effect0_run"\)/);
});

test("no batch in the app: no runtime module, plain effect methods", async () => {
  const app = await gen(`export function App() { const [a] = createSignal(1); createEffect(() => console.log(a())); return <text>{a()}</text>; }`);
  assert.equal(app.modules["modules/solid-batch.mjs"], undefined);
  assert.doesNotMatch(app.entry, /SolidBatch/);
});

// ── createSelector ──────────────────────────────────────────────────────────────────────────────
test("createSelector: isSelected(k) is `k === source()` (or fn(k, source())), reactive on the source", async () => {
  const qml = await qmlOf(`export function App() {
    const [sel, setSel] = createSignal(1);
    const isSel = createSelector(sel);
    const near = createSelector(sel, (k, s) => Math.abs(k - s) < 2);
    createEffect(() => console.log(isSel(3)));
    return <For each={[1, 2, 3]}>{(n) => <text classList={{ on: isSel(n), close: near(n) }}>{n}</text>}</For>;
  }`);
  assert.match(qml, /\.concat\(\(modelData === sel\) \? \["on"\] : \[\]\)\.concat\(\(function\(k, s\) \{ return Math\.abs\(k - s\) < 2 \}\)\(modelData, sel\) \? \["close"\] : \[\]\)/);
  assert.deepEqual(handlers(qml), ["__self.Sel→__effect0"]);
});

// ── fail loud ───────────────────────────────────────────────────────────────────────────────────
const rejects = (body: string, msg: RegExp) => assert.rejects(gen(body), msg);
test("fail loud: createRoot", () =>
  rejects(`export function App() { createRoot(() => {}); return <text>x</text>; }`, /createRoot is not supported natively — a component's computations live exactly as long as its QML object/));
test("fail loud: createMutable", () =>
  rejects(`export function App() { const m = createMutable({ n: 0 }); return <button onClick={() => m.n++}>{m.n}</button>; }`, /createMutable is not supported natively — QML bindings cannot observe in-place mutation of a plain object\. Use createStore/));
test("fail loud: a computation created outside the component's top level", () =>
  rejects(`export function App() { const [a] = createSignal(1); onMount(() => { createEffect(() => console.log(a())); }); return <text>x</text>; }`,
    /`createEffect\(…\)`: createEffect is only supported at the top level of a component body/));
test("fail loud: on() outside createEffect/createMemo", () =>
  rejects(`export function App() { const [a] = createSignal(1); const f = on(a, (v) => v); return <text>{f()}</text>; }`,
    /on is only supported at the top level of a component body .* or as createEffect\(on\(…\)\) \/ createMemo\(on\(…\)\)/));
test("fail loud: on() with a non-literal option", () =>
  rejects(`export function App() { const [a] = createSignal(1); const d = true; createEffect(on(a, (v) => v, { defer: d })); return <text>x</text>; }`,
    /the only supported option is a literal `defer: true\|false`/));
test("a user function named like a Solid API is not mistaken for it", async () => {
  const qml = await qmlOf(`function batchOf(x) { return [x]; }
    export function App() { const [a, setA] = createSignal(1); function on(x) { return x + 1; } return <button onClick={() => setA(on(a()))}>{batchOf(a()).length}</button>; }`);
  assert.match(qml, /onClicked: a = on_\(a\)/);
  assert.match(qml, /function on_\(x_\) \{ return x_ \+ 1; \}/);
});
