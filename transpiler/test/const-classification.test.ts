// Regression tests for the component-level const classification miscompile: a const whose
// initializer is a non-literal, non-reactive expression (a call, member access, `new`, …) that is
// read by a render binding only INDIRECTLY — through another such const's own initializer, not
// named in `render` itself — was not promoted to a QML property by analyzeRenderLocals (which only
// scanned `render` directly). It fell through to analyzeSetup's generic bucket and was emitted as a
// plain JS statement inside Component.onCompleted — which runs AFTER the object's own property
// bindings are first evaluated (verified against the real engine: a property binding referencing a
// name only assigned in onCompleted throws, it does not see a stale/undefined value and recover).
// Example (adapted from the task): `const fonts = usableFonts(Qt.fontFamilies()); const opts =
// fonts.map(f => ({value:f,label:f})); return <select options={opts}/>` — `fonts` is read only by
// `opts`'s initializer, never named in `render` directly.
//
// Fix: analyzeRenderLocals (model/symbols.ts) now closes over inter-candidate references (a BFS
// from the render-seeded set, following each candidate's own init), and extends eligibility from
// `const` to a never-reassigned `let` (same snapshot semantics).
//
// A reassigned `let` with a non-literal init FAILS LOUD (no safe representation — it would freeze
// at its first value as a readonly property, or silently drop the reassignment as a plain one).
//
// An initializer that reads a signal/memo/resource/prop does NOT fail loud: Solid evaluates a
// plain const/let ONCE (unlike createMemo, it never re-runs), but this transpiler represents every
// such reference — called or not — as a bare, live QML property read. renderLocalLiveTaint flags
// these; component.ts emits them as a non-readonly `property var` (still gets the right value at
// construction from the same binding) and then FREEZES them with a self-assignment (`x = x`) in
// Component.onCompleted, before any user onMount/effect code — assigning a value to a QML property
// always clears any binding on it (verified against the real engine, including transitively
// through another frozen local that reads it). A context provider's own value-local is the one
// deliberate exception (kept live — see context-classification tests / module-const-positions.ts).
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

test("a const used only through ANOTHER const's initializer (not named in render directly) is promoted", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
function usableFonts(x) { return x.filter((f) => f.length > 0); }
export function App() {
  const fonts = usableFonts(["Arial", "Helvetica"]);
  const opts = fonts.map((f) => ({ value: f, label: f }));
  return <text>{opts.length}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var fonts: usableFonts/);
  assert.match(app.entry, /readonly property var opts: fonts\.map/);
  assert.doesNotMatch(app.entry, /Component\.onCompleted:[^\n]*fonts\s*=/);
});

test("a chain three deep is fully closed over (A <- B <- C <- render)", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App() {
  const a = [1, 2, 3];
  const b = a.map((x) => x * 2);
  const c = b.filter((x) => x > 2);
  return <text>{c.length}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var a: \[1, 2, 3\]/);
  assert.match(app.entry, /readonly property var b: a\.map/);
  assert.match(app.entry, /readonly property var c: b\.filter/);
});

test("an unrelated const (not reachable from render) is NOT promoted — still a setup statement", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App() {
  const used = [1];
  const unused = [2]; // never read anywhere — stays a plain setup local
  return <text>{used.length}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var used: \[1\]/);
  assert.doesNotMatch(app.entry, /readonly property var unused/);
});

test("a never-reassigned `let` with a call-expression init, read by render, is promoted like a const", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
function usableFonts(x) { return x.filter((f) => f.length > 0); }
export function App() {
  let fonts = usableFonts(["Arial", "Helvetica"]);
  return <text>{fonts.length}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var fonts: usableFonts/);
});

test("a never-reassigned `let` reached only through another const's initializer is ALSO promoted", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
function usableFonts(x) { return x.filter((f) => f.length > 0); }
export function App() {
  let fonts = usableFonts(["Arial", "Helvetica"]);
  const opts = fonts.map((f) => ({ value: f, label: f }));
  return <text>{opts.length}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.match(app.entry, /readonly property var fonts: usableFonts/);
  assert.match(app.entry, /readonly property var opts: fonts\.map/);
});

test("a REASSIGNED `let` with a non-literal init, read by render, fails loud (no safe representation)", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
function usableFonts(x) { return x.filter((f) => f.length > 0); }
export function App() {
  let fonts = usableFonts(["Arial", "Helvetica"]);
  fonts = fonts.slice(0, 1);
  return <text>{fonts.length}</text>;
}
`;
  await assert.rejects(() => generate(src, "t.tsx"), /let fonts: reassigned later/);
});

test("a const initializer that reads a signal is a non-readonly property, frozen once in onCompleted", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App() {
  const [count, setCount] = createSignal(1);
  const doubled = count() * 2;
  return <text>{doubled}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  // NOT readonly — it must accept the later self-assignment that freezes it.
  assert.match(app.entry, /(?<!readonly )property var doubled: count \* 2/);
  assert.doesNotMatch(app.entry, /readonly property var doubled/);
  // Frozen before any other onCompleted code, inside the SAME onCompleted block.
  assert.match(app.entry, /Component\.onCompleted: \{ doubled = doubled;/);
});

test("a const reading a signal ONLY transitively (through another promoted const) is ALSO frozen, in declaration order", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App() {
  const [count, setCount] = createSignal(1);
  const doubled = count() * 2;
  const label = "n=" + doubled;
  return <text>{label}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.doesNotMatch(app.entry, /readonly property var doubled/);
  assert.doesNotMatch(app.entry, /readonly property var label/);
  // `doubled` (the direct signal reader) is frozen before `label` (which only reads `doubled`).
  assert.match(app.entry, /Component\.onCompleted: \{ doubled = doubled; label = label;/);
});

test("a const reading a prop is ALSO frozen (props are live QML properties too, not just signals)", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App(props) {
  const doubledCount = props.count * 2;
  return <text>{doubledCount}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.doesNotMatch(app.entry, /readonly property var doubledCount/);
  assert.match(app.entry, /Component\.onCompleted: \{ doubledCount = doubledCount;/);
});

test("a plain const NOT read by render at all, even if it reads a signal, does not throw (analyzeSetup owns it)", async () => {
  const src = `
import { text } from "../src/solid-qml/runtime";
export function App() {
  const [count, setCount] = createSignal(1);
  const sideEffectOnly = count() * 2; // never read by render
  console.log(sideEffectOnly);
  return <text>{count()}</text>;
}
`;
  const app = await generate(src, "t.tsx");
  assert.doesNotMatch(app.entry, /readonly property var sideEffectOnly/);
});
