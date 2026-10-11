// Keyboard & accessibility (release-plan M5): tabIndex/role/aria-* on a plain element (<div>/<text>)
// map to the SAME native semantics a Templates control gets for free — see docs/keyboard-accessibility.md.
// These tests pin down each row of that mapping table individually (attrs-contract.test.ts's sweep
// only proves nothing is silently dropped; it does not check WHAT a value maps to).
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

test("tabIndex={0} joins the Tab chain, guarded like every native widget's activeFocusOnTab", async () => {
  const r = await emit(`<div tabIndex={0}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /activeFocusOnTab: typeof solidTabstop !== "undefined" && solidTabstop \? solidTabstop\.enabled : true/);
  assert.equal(r.warnings.length, 0);
});

test("tabIndex={-1} is programmatic-only: no activeFocusOnTab line (ref.focus\\(\\) still works)", async () => {
  const r = await emit(`<div tabIndex={-1}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /activeFocusOnTab/);
});

test("a positive tabIndex is treated like 0 (DOM order) and warns once", async () => {
  const r = await emit(`<div tabIndex={3}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /activeFocusOnTab:/);
  assert.ok(r.warnings.some((w) => /tabIndex=\{3\}: positive values are treated like 0/.test(w)), r.warnings.join("\n"));
});

test("role=\"button\" maps to Accessible.role: Accessible.Button", async () => {
  const r = await emit(`<div role="button"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.role: Accessible\.Button/);
});

for (const [role, qrole] of [
  ["button", "Button"], ["link", "Link"], ["checkbox", "CheckBox"], ["switch", "Switch"],
  ["tab", "PageTab"], ["tablist", "PageTabList"], ["menuitem", "MenuItem"], ["listitem", "ListItem"],
  ["list", "List"], ["heading", "Heading"], ["dialog", "Dialog"], ["alert", "AlertMessage"],
  ["img", "Graphic"], ["textbox", "EditableText"],
] as const) {
  test(`role="${role}" → Accessible.${qrole}`, async () => {
    const r = await emit(`<div role="${role}"><text>x</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.match(r.out!, new RegExp(`Accessible\\.role: Accessible\\.${qrole}\\b`));
  });
}

test("an unknown role warns once and emits NO Accessible.role line", async () => {
  const r = await emit(`<div role="nonsense"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /Accessible\.role/);
  assert.ok(r.warnings.some((w) => /role="nonsense": unknown ARIA role/.test(w)), r.warnings.join("\n"));
});

test("aria-label maps to Accessible.name, reactively bound", async () => {
  const r = await emit(`<div aria-label={s()}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.name: s/);
});

test("aria-label as a literal still becomes a live binding (not frozen at emit time)", async () => {
  const r = await emit(`<div aria-label="Close"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.name: "Close"/);
});

test("aria-description is static text → Accessible.description", async () => {
  const r = await emit(`<div aria-description="helper text"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.description: "helper text"/);
});

test("aria-describedby (a static string here, no live DOM id lookup) also becomes Accessible.description", async () => {
  const r = await emit(`<div aria-describedby="helper text"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.description: "helper text"/);
});

for (const [attr, prop] of [
  ["aria-checked", "checked"], ["aria-pressed", "pressed"], ["aria-selected", "selected"],
] as const) {
  test(`${attr} → Accessible.${prop}, reactive`, async () => {
    const r = await emit(`<div ${attr}={s() > 0}><text>x</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.match(r.out!, new RegExp(`Accessible\\.${prop}: `));
  });
  test(`${attr}="true"/"false" string literal is normalised to a real boolean`, async () => {
    const r = await emit(`<div ${attr}="true"><text>x</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.match(r.out!, new RegExp(`Accessible\\.${prop}: true\\b`));
  });
}

test('aria-hidden="true" maps to Accessible.ignored: true', async () => {
  const r = await emit(`<div aria-hidden="true"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Accessible\.ignored: true/);
});

test("aria-disabled drives `enabled` directly (the standard disabled a11y state too)", async () => {
  const r = await emit(`<div aria-disabled={s() > 0}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /enabled: !\(/);
});

test('role="button" + onClick activates on Enter (press) and Space (on RELEASE, matching the browser contract)', async () => {
  const r = await emit(`<div role="button" tabIndex={0} onClick={() => setS(1)}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.match(r.out!, /Keys\.onPressed: \(event\) => \{[\s\S]*?Qt\.Key_Return \|\| event\.key === Qt\.Key_Enter\)[\s\S]*?\}/);
  assert.match(r.out!, /Keys\.onPressed: \(event\) => \{[\s\S]*?if \(event\.key === Qt\.Key_Space\) \{ event\.accepted = true; \}/);
  assert.match(r.out!, /Keys\.onReleased: \(event\) => \{ if \(event\.key === Qt\.Key_Space\)/);
});

for (const role of ["link", "menuitem", "tab"] as const) {
  test(`role="${role}" + onClick activates on Enter AND Space on PRESS (no onReleased)`, async () => {
    const r = await emit(`<div role="${role}" onClick={() => setS(1)}><text>x</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.match(r.out!, /Keys\.onPressed: \(event\) => \{[^}]*Qt\.Key_Return[\s\S]*Qt\.Key_Space\)[\s\S]*\}/);
    assert.doesNotMatch(r.out!, /Keys\.onReleased/);
  });
}

for (const role of ["checkbox", "switch"] as const) {
  test(`role="${role}" + onClick toggles on Space alone (no Enter activation)`, async () => {
    const r = await emit(`<div role="${role}" onClick={() => setS(1)}><text>x</text></div>`);
    assert.ok(!r.err, r.err ?? "");
    assert.match(r.out!, /Keys\.onPressed: \(event\) => \{ if \(event\.key === Qt\.Key_Space\)/);
    assert.doesNotMatch(r.out!, /Key_Return/);
    assert.doesNotMatch(r.out!, /Keys\.onReleased/);
  });
}

test("role=\"button\" WITHOUT onClick gets no keyboard activation statements (nothing to activate)", async () => {
  const r = await emit(`<div role="button"><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /Keys\.onPressed/);
  assert.doesNotMatch(r.out!, /Keys\.onReleased/);
});

test("role=\"heading\" + onClick gets no Enter/Space activation (not an activatable role)", async () => {
  const r = await emit(`<div role="heading" onClick={() => setS(1)}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /Keys\.onPressed/);
});

test("onKeyDown and role-based activation share ONE Keys.onPressed handler (QML allows only one)", async () => {
  const r = await emit(`<div role="button" onClick={() => setS(1)} onKeyDown={(e) => setS(2)}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  const matches = [...r.out!.matchAll(/Keys\.onPressed:/g)];
  assert.equal(matches.length, 1, "exactly one Keys.onPressed line");
});

test("a plain element with none of tabIndex/role/aria-* gets no extra lines at all (no per-row cost)", async () => {
  const r = await emit(`<div onClick={() => setS(1)}><text>x</text></div>`);
  assert.ok(!r.err, r.err ?? "");
  assert.doesNotMatch(r.out!, /Accessible\./);
  assert.doesNotMatch(r.out!, /activeFocusOnTab/);
  assert.doesNotMatch(r.out!, /Keys\.on(Pressed|Released)/);
});

test("role on a tag that does NOT consume it (e.g. <button>) stays warn-only, not fought with the control's own role", async () => {
  const src = `import { createSignal } from "solid-js";
    export function C(){ const [s,setS]=createSignal(0); return <div><button role="link" onClick={() => setS(1)}>x</button></div>; }`;
  const { ast } = await normalize(src, "c.tsx");
  const render = findRender(ast)!;
  const fn = (ast as t.File).program.body.find(
    (n): n is t.ExportNamedDeclaration => t.isExportNamedDeclaration(n) && !!n.declaration && t.isFunctionDeclaration(n.declaration),
  )?.declaration as t.FunctionDeclaration;
  const scope: Scope = { table: analyzeSignals(fn), mode: "binding", usedWidgets: {} as Scope["usedWidgets"] };
  resetAttrWarnings();
  const warnings: string[] = [];
  const ow = console.warn;
  console.warn = (msg?: unknown) => { warnings.push(String(msg)); };
  let out: string;
  try { out = emitQml(render, scope).join("\n"); } finally { console.warn = ow; }
  assert.doesNotMatch(out, /Accessible\.role/);
  assert.ok(warnings.some((w) => /<button role=…>: web-only attribute, no native effect/.test(w)), warnings.join("\n"));
});
