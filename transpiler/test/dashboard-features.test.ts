// Regression tests for the transpiler holes the dashboard stress test exposed:
// operator-precedence parens, module-level const data tables (QML forbids upper-case
// property names), onClick/hover on plain divs, and <Show> inside a <For> delegate.
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

const wrap = (body: string, extra = "") => `
import { createSignal, For, Show } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
${extra}
export function App() {
  const [n, setN] = createSignal(0);
  return (${body});
}
`;

test("binary precedence survives emission: (n() + 2) % 3", async () => {
  const app = await generate(wrap(`<button onClick={() => setN((n() + 2) % 3)}>x</button>`), "t.tsx");
  assert.match(app.entry, /onClicked: n = \(n \+ 2\) % 3/);
});

test("equal precedence on the right keeps grouping: a - (b - c)", async () => {
  const app = await generate(wrap(`<button onClick={() => setN(n() - (2 - 1))}>x</button>`), "t.tsx");
  assert.match(app.entry, /onClicked: n = n - \(2 - 1\)/);
});

test("module-level const arrays become __const_ properties (QML forbids upper-case)", async () => {
  const app = await generate(
    wrap(`<div class="l"><For each={ITEMS}>{(it) => <text>{it.label}</text>}</For></div>`,
         `const ITEMS = [{ label: "a" }, { label: "b" }];`),
    "t.tsx");
  assert.match(app.entry, /readonly property var __const_ITEMS: \[/);
  assert.match(app.entry, /model: __const_ITEMS/);
  assert.doesNotMatch(app.entry, /property var ITEMS/); // upper-case property would not compile
});

test("onClick on a plain div emits a hover-tracked MouseArea + cssState", async () => {
  const app = await generate(wrap(`<div class="item" onClick={() => setN(1)}>x</div>`), "t.tsx");
  assert.match(app.entry, /cssState: __hover0\.containsMouse \? \["hover"\] : \[\]/);
  assert.match(app.entry, /hoverEnabled: true/);
  assert.match(app.entry, /onClicked: n = 1/);
});

test("<Show> inside a <For> delegate gates on the item", async () => {
  const app = await generate(
    wrap(`<div class="l"><For each={ITEMS}>{(it) => <div class="row"><Show when={it.hot}><div class="flag" /></Show></div>}</For></div>`,
         `const ITEMS = [{ hot: true }, { hot: false }];`),
    "t.tsx");
  assert.match(app.entry, /visible: !!\(modelData\.hot\)/);
});
