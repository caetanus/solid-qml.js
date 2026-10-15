// Inline `style` on a CSS-engine box → the engine's `inlineStyle` (emit/inlineStyle.ts). The engine side
// (precedence over classes and :hover, var(), relayout, the per-identity cache) is covered by the engine's
// own `inlineStyle*` tests in vendor/qml-css-engine/tests/test_qml_css.cpp.
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";
import { kebabCssProperty, cssNumber } from "../src/emit/inlineStyle.ts";

async function qml(body: string, setup = ""): Promise<string> {
  const app = await generate(`import { createSignal, For } from "solid-js";
    export function App(){ const [s, setS] = createSignal(4); ${setup} return ${body}; }`, "s.tsx");
  return [app.entry, ...Object.values(app.components)].join("\n");
}

test("style: camelCase keys become kebab-case; vendor prefixes and custom properties", () => {
  assert.equal(kebabCssProperty("backgroundColor"), "background-color");
  assert.equal(kebabCssProperty("borderTopLeftRadius"), "border-top-left-radius");
  assert.equal(kebabCssProperty("WebkitTransform"), "-webkit-transform");
  assert.equal(kebabCssProperty("msTransform"), "-ms-transform");
  assert.equal(kebabCssProperty("background-color"), "background-color");
  assert.equal(kebabCssProperty("--Accent"), "--Accent");
});

test("style: numbers on length properties get px, unitless ones and 0 do not (React DOM)", () => {
  assert.equal(cssNumber("width", 40), "40px");
  assert.equal(cssNumber("margin-top", -2.5), "-2.5px");
  assert.equal(cssNumber("width", 0), "0");
  assert.equal(cssNumber("opacity", 0.5), "0.5");
  assert.equal(cssNumber("flex-grow", 2), "2");
  assert.equal(cssNumber("z-index", 3), "3");
  assert.equal(cssNumber("line-height", 1.5), "1.5");
  assert.equal(cssNumber("--gap", 4), "4");
});

test("style: an object literal is converted at compile time (keys, px) on a <div>", async () => {
  const out = await qml(`<div class="card" style={{ backgroundColor: "red", width: 40, opacity: 0.5, "margin-top": -3, "--accent": "#123" }}>x</div>`);
  assert.match(out, /cssClass: \["card"\]\n\s+inlineStyle: \(\{ "background-color": "red", "width": "40px", "opacity": "0\.5", "margin-top": "-3px", "--accent": "#123" \}\)/);
});

test("style: a dynamic value keeps a reactive binding (the engine adds px to a number)", async () => {
  const out = await qml(`<div style={{ width: s(), color: s() > 2 ? "red" : "blue" }}>x</div>`);
  assert.match(out, /inlineStyle: \(\{ "width": s, "color": s > 2 \? "red" : "blue" \}\)/);
});

test("style: a string is passed straight through (the engine parses it)", async () => {
  const out = await qml(`<div style="color: red; padding: 4px 8px">x</div>`);
  assert.match(out, /inlineStyle: "color: red; padding: 4px 8px"/);
  const tpl = await qml("<div style={`opacity: 0.5`}>x</div>");
  assert.match(tpl, /inlineStyle: "opacity: 0\.5"/);
});

test("style: a non-literal object is handed to the engine as is (runtime conversion)", async () => {
  const ident = await qml(`<div style={st}>x</div>`, `const st = { backgroundColor: "red", width: 10 };`);
  assert.match(ident, /inlineStyle: st\b/);
  const spread = await qml(`<div style={{ ...st, width: s() }}>x</div>`, `const st = { color: "red" };`);
  assert.match(spread, /inlineStyle: Object\.assign\(\{\}, st, \(\{ width: s \}\)\)/);
  const cond = await qml(`<div style={s() > 1 ? "opacity: 1" : { opacity: 0.2 }}>x</div>`);
  assert.match(cond, /inlineStyle: s > 1 \? "opacity: 1" : \(\{ opacity: 0\.2 \}\)/);
});

test("style: on text tags (W.Text) too", async () => {
  const out = await qml(`<div><span style={{ fontWeight: 700, fontSize: 14 }}>x</span><p style="color: red">y</p></div>`);
  assert.match(out, /W\.Text \{\n\s+inlineStyle: \(\{ "font-weight": "700", "font-size": "14px" \}\)\n\s+cssPrimitive: "span"/);
  assert.match(out, /W\.Text \{\n\s+inlineStyle: "color: red"\n\s+cssPrimitive: "p"/);
});

test("style: inside a <For> row it binds to the row (a palette)", async () => {
  const out = await qml(`<div class="palette"><For each={colors}>{(c) => <div class="swatch" style={{ background: c }} />}</For></div>`,
    `const colors = ["#e53935", "#43a047", "#1e88e5"];`);
  assert.match(out, /cssClass: \["swatch"\]\n\s+inlineStyle: \(\{ "background": (modelData|c|[\w.]+) \}\)/);
});

test("style: an element without style emits no inlineStyle line (unchanged output)", async () => {
  const out = await qml(`<div class="a"><text>x</text></div>`);
  assert.doesNotMatch(out, /inlineStyle/);
});

test("style: native widgets keep rejecting it with a clear error", async () => {
  for (const el of [`<button style={{ opacity: 0.5 }}>x</button>`, `<input style="color: red" />`,
    `<img src="a.png" style={{ width: 4 }} />`, `<select style="x: y" />`]) {
    await assert.rejects(qml(el), /does not support `style` natively: inline styles apply only to CSS-engine boxes/, el);
  }
});
