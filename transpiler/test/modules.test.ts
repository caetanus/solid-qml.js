import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

// Release-plan M2 ("no silent miscompiles"), item 3: module-level code. A helper imported from the
// app's own non-component module, a module const initialised with `new …`/a RegExp/a call, an
// exported helper, an async module function — all used to be undefined at runtime without an
// error. They are now emitted; what has no per-component equivalent fails loud.

const HEAD = `import { createSignal } from "solid-js";\n`;
async function gen(body: string, files: Record<string, string> = {}) {
  return generate(HEAD + body, "/virtual/app.tsx", {
    readFile: async (p) => { if (p in files) return files[p]; throw new Error(`no file ${p}`); },
  });
}
const all = (app: { entry: string; components: Record<string, string> }) => [app.entry, ...Object.values(app.components)].join("\n");
const rejects = (body: string, msg: RegExp, files: Record<string, string> = {}) => assert.rejects(gen(body, files), msg);
const button = (expr: string) => `export function App() { const [s, setS] = createSignal(1); return <button onClick={() => setS(${expr})}>{s()}</button>; }`;

// ── helpers imported from the app's own modules ─────────────────────────────────────────────────
test("a helper imported from a plain relative module runs as a shared JS module (named, default, namespace, transitive)", async () => {
  const app = await gen(`import { twice, LIMIT } from "./util"; import fmt from "./fmt"; import * as U from "./util";
    export function App() { const [s, setS] = createSignal(1); return <button onClick={() => setS(twice(s()) + U.LIMIT)}>{fmt(s())}{LIMIT}</button>; }`, {
    "/virtual/util.ts": `import { mul } from "./mul"; export const LIMIT: number = 10; export function twice(x: number) { return mul(x, 2); }`,
    "/virtual/mul.ts": `export const mul = (a, b) => a * b;`,
    "/virtual/fmt.ts": `export default function fmt(n) { return "#" + n; }`,
  });
  assert.match(app.entry, /^import "modules\/util_\w+\.mjs" as Js0$/m);
  assert.match(app.entry, /^import "modules\/fmt_\w+\.mjs" as Js1$/m);
  assert.match(app.entry, /onClicked: s = Js0\.twice\(s\) \+ Js0\.LIMIT/);
  assert.match(app.entry, /text: "" \+ \(Js1\.Default\(s\)\) \+ \(Js0\.LIMIT\)/);
  const util = Object.entries(app.modules).find(([k]) => k.startsWith("modules/util_"))![1];
  assert.match(util, /import \{ mul \} from "\.\/mul_\w+\.mjs";/);
  assert.match(util, /export var LIMIT = 10;/, "types stripped");
  assert.ok(Object.keys(app.modules).some((k) => k.startsWith("modules/mul_")), "the transitive import is mirrored too");
});

test("a component import is still followed as a component (not mirrored)", async () => {
  const app = await gen(`import { Card } from "./card"; export function App() { return <Card />; }`,
    { "/virtual/card.tsx": `export function Card() { return <text>c</text>; }` });
  assert.ok(app.components.Card);
  assert.deepEqual(Object.keys(app.modules), []);
});

test("fail loud: importing a non-component binding from a module with JSX, with Solid state, or not JS", async () => {
  await rejects(`import { fmt } from "./ui"; ${button("fmt(s())")}`,
    /import \{ fmt \} from "\.\/ui": ui\.tsx also contains JSX \(components\) — a non-component binding can only be imported from a plain module/,
    { "/virtual/ui.tsx": `export function fmt(x) { return x; } export function Card() { return <text>c</text>; }` });
  await rejects(`import { count } from "./store"; ${button("count()")}`,
    /store\.ts imports "solid-js" — Solid state created at module level would not be shared or tracked natively/,
    { "/virtual/store.ts": `import { createSignal } from "solid-js"; export const [count, setCount] = createSignal(0);` });
  await rejects(`import { f } from "./nope"; ${button("f()")}`, /import \{ f \} from "\.\/nope": cannot resolve \.\/nope/);
  await rejects(`import data from "./data.json"; ${button("data.n")}`, /data\.json is not a JS\/TS module/, { "/virtual/data.json": `{"n": 1}` });
});

// ── module-level consts ─────────────────────────────────────────────────────────────────────────
test("module consts with any initializer are emitted: RegExp, new Map, calls, other consts and helpers", async () => {
  const app = await gen(`const RE = /a+/g;
    const M = new Map([["k", 3]]);
    function build(n) { return Array.from({ length: n }, (_, i) => i * 2); }
    const TABLE = build(3);
    const SIZE = TABLE.length + M.get("k");
    export function App() { const [s, setS] = createSignal("aab"); return <button onClick={() => setS(s().replace(RE, "-"))}>{s()}{SIZE}</button>; }`);
  assert.match(app.entry, /readonly property var __const_RE: \/a\+\/g/);
  assert.match(app.entry, /readonly property var __const_M: new Map\(\[\["k", 3\]\]\)/);
  assert.match(app.entry, /readonly property var __const_TABLE: build\(3\)/);
  assert.match(app.entry, /readonly property var __const_SIZE: __const_TABLE\.length \+ __const_M\.get\("k"\)/);
  assert.match(app.entry, /function build\(n\) \{/);
  assert.match(app.entry, /onClicked: s = s\.replace\(__const_RE, "-"\)/);
});

test("exported helpers/consts of the component's own module are emitted (they were undefined)", async () => {
  const app = await gen(`export const UNIT = "px"; export function px(n) { return n + UNIT; }
    export function App() { const [s] = createSignal(1); return <text>{px(s())}</text>; }`);
  assert.match(app.entry, /readonly property var __const_UNIT: "px"/);
  assert.match(app.entry, /function px\(n\) \{ return n \+ __const_UNIT; \}/);
});

test("an async module function is callable from a handler (Babel lowers it to a const)", async () => {
  const app = await gen(`async function save(x) { const r = await fetch("https://example.com/" + x); return r.ok; }
    export function App() { const [s, setS] = createSignal(0); return <button onClick={() => save(s())}>{s()}</button>; }`);
  assert.match(app.entry, /readonly property var __const_save: _async\(function\(x_\) \{/);
  assert.match(app.entry, /function _async\(f\) \{/);
  assert.match(app.entry, /onClicked: __const_save\(s\)/);
});

test("fail loud: module-level reactive state, let/var state, JSX constants, destructuring", async () => {
  await rejects(`const [n, setN] = createSignal(0); ${button("n() + 1")}`,
    /module-level createSignal\(…\) \(n, setN\): reactive state created outside a component is not supported natively/);
  await rejects(`let total = 0; ${button("total")}`, /module-level `let total`: module state is not shared between components natively/);
  await rejects(`const ICON = <text>i</text>; export function App() { return <div>{ICON}</div>; }`, /module-level JSX constant `ICON` is not supported natively/);
  await rejects(`const { a, b } = { a: 1, b: 2 }; ${button("a + b")}`, /module-level destructuring `const a, b` is not supported natively/);
});

test("fail loud: a module const mutated in place (each instance has its own copy)", async () => {
  await rejects(`const SEEN = []; export function App() { const [s, setS] = createSignal(0); return <button onClick={() => { SEEN.push(s()); setS(SEEN.length); }}>{s()}</button>; }`,
    /module-level const SEEN is mutated \(\.push\(…\)\) — each component instance gets its own copy/);
  await rejects(`const CACHE = new Map(); function remember(k, v) { CACHE.set(k, v); return v; } ${button("remember(1, 2)")}`,
    /module-level const CACHE is mutated \(\.set\(…\)\)/);
  await rejects(`const CFG = { n: 1 }; ${button("(CFG.n = 2)")}`, /module-level const CFG is mutated \(an assignment to a member\)/);
  // A local of the same name is not the module const.
  const app = await gen(`const LIST = [1]; export function App() { const [s, setS] = createSignal(0); function go() { const LIST = []; LIST.push(1); setS(LIST.length); } return <button onClick={go}>{LIST.length}</button>; }`);
  assert.match(all(app), /readonly property var __const_LIST: \[1\]/);
});
