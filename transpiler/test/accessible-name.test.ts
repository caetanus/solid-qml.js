// Accessible name follow-up to release-plan M5 (docs/keyboard-accessibility.md):
//
// (2) "name from content": a role that allows it (button/link/menuitem/tab/checkbox/switch/
//     listitem/heading) and carries NO aria-label gets no synthesized Accessible.name line at
//     transpile time — the name comes from a RUNTIME descendant-text walk (src/widgets/a11y.cpp's
//     ContentNameAccessible), not a baked expression. This is a deliberate design choice (see the
//     module doc comment there for why): reactive by construction, uniform across static/reactive/
//     dynamic (<For>/<Show>/component) content, zero-cost until an AT client asks. These tests pin
//     the transpiler side of that contract — no spurious line, and an explicit aria-label still wins.
//
// (3) aria-label/aria-description on NATIVE controls (button, every <input> type the transpiler
//     maps, textarea, select) now map to `accessibleName`/`accessibleDescription` on the W.*
//     instance (forwarded by the C++ wrapper onto the inner Templates control — see button.cpp,
//     textinputs.cpp, toggles.cpp, sliders.cpp, spindial.cpp, select.cpp, datefield.cpp), instead of
//     being silently warn-only. Radio has no C++ wrapper (a raw T.RadioButton) — set directly.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { resetAttrWarnings } from "../src/emit/attrs.ts";
import type { Scope } from "../src/emit/expr.ts";

type Result = { out?: string; err?: string; warnings: string[] };

async function emit(body: string): Promise<Result> {
  const src = `import { createSignal } from "solid-js";
    export function C(){ const [s,setS]=createSignal(0); return <div>${body}</div>; }`;
  const { ast } = await normalize(src, "c.tsx");
  const render = findRender(ast)!;
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body)
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding", usedWidgets: {} as Scope["usedWidgets"] };
  resetAttrWarnings();
  const warnings: string[] = [];
  const ow = console.warn;
  console.warn = (msg?: unknown) => { warnings.push(String(msg)); };
  try { return { out: emitQml(render, scope).join("\n"), warnings }; }
  catch (e) { return { err: (e as Error).message, warnings }; }
  finally { console.warn = ow; }
}

// ── (2) name from content: no synthesized line, aria-label still overrides ──────────────────────

for (const role of ["button", "link", "menuitem", "tab", "checkbox", "switch", "listitem", "heading"] as const) {
  test(`role="${role}" with text content and NO aria-label emits no Accessible.name line (runtime fallback owns it)`, async () => {
    const r = await emit(`<div role="${role}"><text>Pastas</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.doesNotMatch(r.out!, /Accessible\.name/);
  });
}

test('role="button" WITH aria-label still emits the explicit Accessible.name (author override wins)', async () => {
  const r = await emit(`<div role="button" aria-label="Close"><text>Pastas</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.name: "Close"/);
});

test('role="list" (NOT in the name-from-content allow-list) also emits no Accessible.name line', async () => {
  const r = await emit(`<div role="list"><text>Pastas</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /Accessible\.name/);
});

// ── (3) aria-label/aria-description on native controls ──────────────────────────────────────────

test("<button aria-label> forwards accessibleName onto W.Button (reactive binding)", async () => {
  const r = await emit(`<button aria-label={s()}>Save</button>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.Button \{[\s\S]*accessibleName: s\b/);
});

test("<button aria-description> forwards accessibleDescription", async () => {
  const r = await emit(`<button aria-description="Saves the document">Save</button>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /accessibleDescription: "Saves the document"/);
  assert.equal(r.warnings.length, 0);
});

test('<input aria-label> forwards accessibleName onto W.TextField', async () => {
  const r = await emit(`<input aria-label="Search" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.TextField \{[\s\S]*accessibleName: "Search"/);
  assert.equal(r.warnings.length, 0);
});

test('<textarea aria-label> forwards accessibleName onto W.TextArea', async () => {
  const r = await emit(`<textarea aria-label="Comments" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.TextArea \{[\s\S]*accessibleName: "Comments"/);
  assert.equal(r.warnings.length, 0);
});

test('<input type="checkbox" aria-label> forwards accessibleName onto W.Checkbox', async () => {
  const r = await emit(`<input type="checkbox" aria-label="Accept terms" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.Checkbox \{[\s\S]*accessibleName: "Accept terms"/);
});

test('<input type="checkbox" role="switch" aria-label> forwards accessibleName onto W.Toggle', async () => {
  const r = await emit(`<input type="checkbox" role="switch" aria-label="Mute" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.Toggle \{[\s\S]*accessibleName: "Mute"/);
});

test('<input type="radio" aria-label> sets Accessible.name directly on T.RadioButton (no C++ wrapper)', async () => {
  const r = await emit(`<input type="radio" name="g" aria-label="Small" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /T\.RadioButton \{[\s\S]*Accessible\.name: "Small"/);
});

test('<input type="range" aria-label> forwards accessibleName onto W.Slider', async () => {
  const r = await emit(`<input type="range" aria-label="Volume" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.Slider \{[\s\S]*accessibleName: "Volume"/);
});

test('<input type="number" aria-label> forwards accessibleName onto W.SpinBox', async () => {
  const r = await emit(`<input type="number" aria-label="Quantity" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.SpinBox \{[\s\S]*accessibleName: "Quantity"/);
});

test('<input type="date" aria-label> forwards accessibleName onto W.DateField', async () => {
  const r = await emit(`<input type="date" aria-label="Due date" />`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.DateField \{[\s\S]*accessibleName: "Due date"/);
});

test('<select aria-label> forwards accessibleName onto W.Select', async () => {
  const r = await emit(`<select aria-label="Country"><option value="br">Brazil</option></select>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /W\.Select \{[\s\S]*accessibleName: "Country"/);
});

test("aria-label on native controls no longer warns as a web-only attribute", async () => {
  const r = await emit(`<button aria-label="Save">x</button>`);
  assert.ok(!r.err, r.err ?? "");
  assert.equal(r.warnings.filter((w) => /aria-label/.test(w)).length, 0, r.warnings.join("\n"));
});
