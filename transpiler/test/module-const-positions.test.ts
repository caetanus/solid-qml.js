// Regression tests for the module-const rewrite bug found via solid-player: a module-level
// `const NAME = <literal>` is surfaced as `readonly property var __const_NAME: [...]` and every
// REFERENCE to NAME inside a component must be rewritten to `__const_NAME` — but the rewrite was
// only wired into the render-tree scope, not into every other Scope the emitter builds for
// component-level declarations (prop defaults, signal/store/memo inits, render-local aliases,
// provider fn members). This file pins down each position, plus shadowing (a local/param with
// the same name as a module const must NOT be rewritten).
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

test("a component-level const aliasing a module const is rewritten (the reported bug)", async () => {
  const src = `
import { For } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
const RULE_FIELDS = [{ k: "a" }, { k: "b" }];
export function App() {
  const ruleFields = RULE_FIELDS;
  return (
    <div class="l">
      <For each={ruleFields}>{(it) => <text>{it.k}</text>}</For>
    </div>
  );
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var ruleFields: __const_RULE_FIELDS/);
  assert.doesNotMatch(app.entry, /readonly property var ruleFields: RULE_FIELDS\b/);
});

test("createSignal's initial value rewrites a module const reference", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const DEFAULT_COUNT = 5;
export function App() {
  const [n] = createSignal(DEFAULT_COUNT);
  return <text>{n()}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /property var n: __const_DEFAULT_COUNT/);
});

test("createMemo's expression body rewrites a module const reference", async () => {
  const src = `
import { createSignal, createMemo } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const MULTIPLIER = 3;
export function App() {
  const [n] = createSignal(1);
  const doubled = createMemo(() => n() * MULTIPLIER);
  return <text>{doubled()}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var doubled: n \* __const_MULTIPLIER/);
});

test("createMemo's block body rewrites a module const reference", async () => {
  const src = `
import { createSignal, createMemo } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const MULTIPLIER = 3;
export function App() {
  const [n] = createSignal(1);
  const doubled = createMemo(() => { const m = MULTIPLIER; return n() * m; });
  return <text>{doubled()}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /var m = __const_MULTIPLIER;/);
});

test("a mergeProps default value rewrites a module const reference", async () => {
  const src = `
import { mergeProps } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
const DEFAULT_GREETING = "Hello";
function Greeting(props) {
  const merged = mergeProps({ greeting: DEFAULT_GREETING }, props);
  return <text>{merged.greeting}</text>;
}
export function App() {
  return <div><Greeting /></div>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.components.Greeting, /property var greeting: __const_DEFAULT_GREETING/);
});

test("a context provider's fn member rewrites a module const reference", async () => {
  const src = `
import { createContext, useContext, createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
const STEP = 5;
const CounterContext = createContext();
function CounterProvider(props) {
  const [count, setCount] = createSignal(props.count || 0);
  const counter = { count, increment: () => setCount(count() + STEP) };
  return (
    <CounterContext.Provider value={counter}>
      {props.children}
    </CounterContext.Provider>
  );
}
function Display() {
  const counter = useContext(CounterContext);
  return <text>Count: {counter.count()}</text>;
}
export function App() {
  return (
    <CounterProvider count={5}>
      <div class="app"><Display /></div>
    </CounterProvider>
  );
}
`;
  const app = await generate(src, "t.tsx");
  const provider = app.components.CounterProvider;
  assert.ok(provider, "CounterProvider should be emitted as its own component");
  assert.match(provider, /__const_STEP/);
  assert.doesNotMatch(provider, /\bSTEP\b(?!_)|(?<!_)\bSTEP\b/);
});

test("a local helper function whose own param shadows a module const is NOT rewritten inside its body", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
const FACTOR = 2;
export function App() {
  function calc(FACTOR) { return FACTOR * 10; }
  return <text>{calc(FACTOR)}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // The call argument (outer FACTOR) is the module const — rewritten.
  assert.match(app.entry, /calc\(__const_FACTOR\)/);
  // The function body's own param (shadowing FACTOR) stays bare — NOT rewritten.
  assert.match(app.entry, /function calc\(FACTOR\) \{ return FACTOR \* 10; \}/);
});

test("a <For> delegate param that shadows a module const name resolves to the row, not the const", async () => {
  const src = `
import { For } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
const ITEM = ["a", "b"];
export function App() {
  return (
    <div class="l">
      <For each={ITEM}>{(ITEM) => <text>{ITEM}</text>}</For>
    </div>
  );
}
`;
  const app = await generate(src, "t.tsx");
  // The each= expression (outer ITEM) is the module const — rewritten.
  assert.match(app.entry, /model: __const_ITEM/);
  // Inside the delegate, the param ITEM must resolve to the row data, never to __const_ITEM.
  assert.doesNotMatch(app.entry, /text\s*\{[^}]*__const_ITEM/s);
});
