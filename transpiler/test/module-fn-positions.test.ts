// Regression tests for the module-level HELPER FUNCTION bug (the function analog of
// module-const-positions.test.ts / commit c639946): a module-level `function f(...) {...}` or
// `const f = (...) => ...` that is NOT a component is never emitted into the QML type of any
// component that calls it — the call survives in the generated binding/handler/effect text, but
// the function itself is dropped. No transpile-time error; the generated app throws
// "ReferenceError: <name> is not defined" the moment the binding first evaluates (reported via the
// office app: `function thousands(n) {…}` + `<text>{thousands(count())}</text>`).
//
// Fix: module-level helper functions are collected (analyzeModuleFunctions), selected per
// component via the TRANSITIVE closure of what that component references (collectModuleFunctionClosure
// — a referenced helper may call another helper or read a module const), and emitted as QML object
// methods on every component that needs them — same mechanism as a component's own local helpers,
// but shared by name across every component in the file (each gets its own copy/properties, since
// each component is its own QML type).
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

test("a module-level function used in a text binding is emitted into the component", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
function thousands(n) {
  return String(n) + "!";
}
export function App() {
  const [count] = createSignal(5);
  return <text>{thousands(count())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function thousands\(n\) \{ return String\(n\) \+ "!"; \}/);
  assert.match(app.entry, /text: "" \+ \(thousands\(count\)\)/);
});

test("a module-level function called from a handler is emitted into the component", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
function shout(s) { return s + "!!!"; }
export function App() {
  const [msg, setMsg] = createSignal("hi");
  return (
    <div>
      <text>{msg()}</text>
      <button onClick={() => setMsg(shout(msg()))}>Shout</button>
    </div>
  );
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function shout\(s\) \{ return s \+ "!!!"; \}/);
  assert.match(app.entry, /onClicked: msg = shout\(msg\)/);
});

test("a bare module-level function reference as a handler is emitted and called (like a local helper)", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
function bump(setter, n) { setter(n + 1); }
export function App() {
  const [count, setCount] = createSignal(0);
  const inc = () => { bump(setCount, count()); };
  return (
    <div>
      <text>{count()}</text>
      <button onClick={inc}>Inc</button>
    </div>
  );
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function bump\(setter, n\) \{ setter\(n \+ 1\); \}/);
});

test("transitive: a module helper calling another module helper that reads a module const", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const UNIT = " kg";
function withUnit(s) { return s + UNIT; }
function thousands(n) { return withUnit(String(n)); }
export function App() {
  const [weight] = createSignal(10);
  return <text>{thousands(weight())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // Both helpers are emitted...
  assert.match(app.entry, /function thousands\(n\) \{ return withUnit\(String\(n\)\); \}/);
  assert.match(app.entry, /function withUnit\(s\) \{ return s \+ __const_UNIT; \}/);
  // ...and the const it transitively needs is surfaced too, even though App() itself never
  // mentions UNIT directly.
  assert.match(app.entry, /readonly property var __const_UNIT: " kg"/);
});

test("a module-level const arrow function (`const f = (...) => ...`) is emitted as a helper", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const double = (n) => n * 2;
export function App() {
  const [n] = createSignal(3);
  return <text>{double(n())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function double\(n\) \{ return n \* 2; \}/);
  assert.match(app.entry, /text: "" \+ \(double\(n\)\)/);
});

test("a module-level block-bodied const arrow function is emitted as a helper", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const describe = (n) => { const label = n > 0 ? "pos" : "neg"; return label; };
export function App() {
  const [n] = createSignal(3);
  return <text>{describe(n())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function describe\(n\) \{ var label = n > 0 \? "pos" : "neg"; return label; \}/);
});

test("two components in the same file both reference the same module helper (multi-file output)", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
function shout(s) { return s + "!"; }
function First() {
  const [n] = createSignal(1);
  return <text>{shout(String(n()))}</text>;
}
export function App() {
  const [n] = createSignal(2);
  return (
    <div>
      <First />
      <text>{shout(String(n()))}</text>
    </div>
  );
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function shout\(s\) \{ return s \+ "!"; \}/);
  assert.ok(app.components.First, "First is emitted as its own component");
  assert.match(app.components.First, /function shout\(s\) \{ return s \+ "!"; \}/);
});

test("a component's own param shadowing a module helper name is NOT rewritten to the helper", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
function thousands(n) { return String(n) + "k"; }
export function App() {
  function thousands(label) { return "local:" + label; }
  return <text>{thousands("x")}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // The component's OWN local helper of the same name wins — the module-level one is never
  // pulled in (no duplicate `function thousands` declaration on the type).
  const matches = app.entry.match(/function thousands\(/g) ?? [];
  assert.equal(matches.length, 1, "only the component's own local helper is emitted, not a duplicate");
  assert.match(app.entry, /function thousands\(label\) \{ return "local:" \+ label; \}/);
});

test("a module helper's own param shadowing a module const is NOT rewritten inside its body", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
const FACTOR = 2;
function scaleBy(FACTOR) { return FACTOR * 10; }
export function App() {
  const [n] = createSignal(1);
  return <text>{scaleBy(FACTOR) + n()}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // The call site (App's own reference to the module const) is rewritten...
  assert.match(app.entry, /scaleBy\(__const_FACTOR\)/);
  // ...but inside scaleBy's own body, its param (same name) stays bare.
  assert.match(app.entry, /function scaleBy\(FACTOR\) \{ return FACTOR \* 10; \}/);
});

test("a module helper NAMED like a QML/QtQuick builtin property is safeName-escaped consistently at its declaration AND every call site (same mechanism as any other identifier)", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
function scale(n) { return n * 2; }
export function App() {
  const [n] = createSignal(1);
  return <text>{scale(n())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // "scale" collides with the QtQuick Item transform property — safeName appends "_" to BOTH
  // the declaration and every call site, so the method never shadows (or is shadowed by) the
  // real `scale` property.
  assert.match(app.entry, /function scale_\(n\) \{ return n \* 2; \}/);
  assert.match(app.entry, /text: "" \+ \(scale_\(n\)\)/);
  assert.doesNotMatch(app.entry, /[^_]\bscale\(/);
});

test("a module helper that closes over mutable module `let` state fails loud at transpile time", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
let total = 0;
function addToTotal(n) { total += n; return total; }
export function App() {
  return <text>{addToTotal(5)}</text>;
}
`;
  await assert.rejects(
    () => generate(src, "t.tsx"),
    (err: Error) => {
      assert.match(err.message, /addToTotal/);
      assert.match(err.message, /total/);
      return true;
    },
  );
});

test("an unreferenced module helper is not emitted (dead code stays dead)", async () => {
  const src = `
import { createSignal } from "solid-js";
import { text } from "../src/solid-qml/runtime";
function used(n) { return n; }
function unused(n) { return n * 99; }
export function App() {
  const [n] = createSignal(1);
  return <text>{used(n())}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /function used\(n\)/);
  assert.doesNotMatch(app.entry, /function unused\(/);
});
