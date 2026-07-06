/** Native HTML-widget group tests: <progress>, <fieldset>/<legend>, <dialog>, <details>/<summary>.
 *
 *  Pattern mirrors test/widgets.test.ts: a local `qml()` helper for direct emitQml calls,
 *  and `qmlType()` for full emitComponentType output (needed for the Templates import check). */

import { test } from "node:test";
import assert from "node:assert/strict";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { emitComponentType } from "../src/emit/component.ts";
import type { Scope } from "../src/emit/expr.ts";
import * as t from "@babel/types";

/** Emit a render tree to QML (no component wrapper, direct emitQml).
 *  Shared counters (inputCounter, hoverCounter) are threaded so multiple widgets in one
 *  render tree get distinct ids — mirrors how emitComponentType creates them. */
async function qml(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const render = findRender(ast);
  if (!render) throw new Error("no render");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  const inputCounter = { n: 0 };
  const hoverCounter = { n: 0 };
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding", inputCounter, hoverCounter };
  return emitQml(render, scope).join("\n");
}

/** Emit a full component type (includes the prepended Templates import when widgets are used). */
async function qmlType(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const file = ast as t.File;
  let fn: t.Function | null = null;
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node)) fn = node;
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  }
  if (!fn) throw new Error("no fn");
  const render = findRender(ast)!;
  return emitComponentType(fn, render, new Map()).join("\n");
}

// ---------------------------------------------------------------------------
// <progress> — structure
// ---------------------------------------------------------------------------

test("progress: wrapper CssFill with cssPrimitive 'progress' and nulled chrome", async () => {
  const out = await qml(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "progress"/);
  assert.match(out, /T\.ProgressBar \{/);
  assert.match(out, /contentItem: null/);
  assert.match(out, /background: null/);
  assert.match(out, /anchors\.fill: parent/);
});

test("progress: track and bar CssRects with visualPosition-driven width", async () => {
  const out = await qml(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /cssClass: \["track"\]/);
  assert.match(out, /cssClass: \["bar"\]/);
  assert.match(out, /width: __input0\.indeterminate \? parent\.width \* 0\.3 : __input0\.visualPosition \* parent\.width/);
});

test("progress: class lands on the wrapper cssClass", async () => {
  const out = await qml(`export function F(){ return <progress class="hw-progress" value={0.5} />; }`);
  assert.match(out, /cssClass: \["hw-progress"\]/);
});

// ---------------------------------------------------------------------------
// <progress> — prop wiring
// ---------------------------------------------------------------------------

test("progress: value={sig()} binds value and does NOT set indeterminate", async () => {
  const out = await qml(`
    export function F() {
      const [pct, setPct] = createSignal(0.25);
      return <progress value={pct()} />;
    }
  `);
  assert.match(out, /value: pct/);
  assert.doesNotMatch(out, /indeterminate: true/);
});

test("progress: max={100} maps to `to: 100`; from is always 0", async () => {
  const out = await qml(`export function F(){ return <progress max={100} value={40} />; }`);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 100/);
});

test("progress: max defaults to 1 (HTML spec)", async () => {
  const out = await qml(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /to: 1/);
});

test("progress: no value prop → indeterminate with looping bar animation", async () => {
  const out = await qml(`export function F(){ return <progress />; }`);
  assert.match(out, /indeterminate: true/);
  assert.doesNotMatch(out, /\n\s*value:/);
  assert.match(out, /NumberAnimation on x \{/);
  assert.match(out, /running: __input0\.indeterminate/);
  assert.match(out, /loops: Animation\.Infinite/);
  assert.match(out, /cssState: __input0\.indeterminate \? \["indeterminate"\] : \[\]/);
});

test("progress: emitting the widget flags the Templates import", async () => {
  const out = await qmlType(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /import QtQuick\.Templates .* as T/);
});

// ---------------------------------------------------------------------------
// <fieldset> / <legend>
// ---------------------------------------------------------------------------

test("fieldset: wrapper CssFill with cssPrimitive 'fieldset' renders children", async () => {
  const out = await qml(`export function F(){ return <fieldset class="fs"><text class="a">hi</text></fieldset>; }`);
  assert.match(out, /cssPrimitive: "fieldset"/);
  assert.match(out, /cssClass: \["fs"\]/);
  assert.match(out, /cssClass: \["a"\]/);
});

test("fieldset: <legend> becomes a CssText (cssPrimitive 'legend', cssClass ['legend'])", async () => {
  const out = await qml(`export function F(){ return <fieldset><legend>Shipping</legend><text>x</text></fieldset>; }`);
  assert.match(out, /Css\.CssText \{/);
  assert.match(out, /cssPrimitive: "legend"/);
  assert.match(out, /cssClass: \["legend"\]/);
  assert.match(out, /text: "Shipping"/);
});

test("fieldset: the first <legend> is spliced FIRST even when authored last", async () => {
  const out = await qml(`export function F(){ return <fieldset><text class="a">x</text><legend>L</legend></fieldset>; }`);
  const legendAt = out.indexOf('cssPrimitive: "legend"');
  const siblingAt = out.indexOf('cssClass: ["a"]');
  assert.ok(legendAt >= 0 && siblingAt >= 0);
  assert.ok(legendAt < siblingAt, "legend must be emitted before the other children");
});

test("legend: author classes join after the built-in 'legend' class", async () => {
  const out = await qml(`export function F(){ return <fieldset><legend class="big">L</legend></fieldset>; }`);
  assert.match(out, /cssClass: \["legend", "big"\]/);
});

test("legend: element children throw a clear transpiler error", async () => {
  await assert.rejects(
    qml(`export function F(){ return <fieldset><legend><span>no</span></legend></fieldset>; }`),
    /legend.*text content only/,
  );
});

// ---------------------------------------------------------------------------
// <dialog> — structure
// ---------------------------------------------------------------------------

const DIALOG_SRC = `
  export function F() {
    const [open, setOpen] = createSignal(false);
    return <dialog class="dlg" open={open()} onClose={() => setOpen(false)}><div class="body"><text>hi</text></div></dialog>;
  }
`;

test("dialog: is a REAL modal Window (Qt.Dialog), not an overlay popup", async () => {
  const out = await qml(DIALOG_SRC);
  // Owner directive: a dialog is a window. No T.Dialog / overlay popup.
  assert.match(out, /Window \{/);
  assert.match(out, /flags: Qt\.Dialog/);
  assert.match(out, /modality: Qt\.WindowModal/);
  assert.match(out, /transientParent: __dialog0W\.Window\.window/);
  assert.match(out, /cssPrimitive: "dialog"/);
  assert.doesNotMatch(out, /T\.Dialog/);
  assert.doesNotMatch(out, /T\.Overlay\.overlay/);
});

test("dialog: window sizes itself to the content's implicit size", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /width: Math\.max\(1, __dialog0Root\.implicitWidth\)/);
  assert.match(out, /height: Math\.max\(1, __dialog0Root\.implicitHeight\)/);
});

test("dialog: open prop drives visible + a RestoreNone Binding (survives self-close)", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /visible: !!\(open\)/);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __dialog0/);
  assert.match(out, /property: "visible"/);
  assert.match(out, /value: !!\(open\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("dialog: onClose handler wires to the window's onClosing", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /onClosing: \{ open = false \}/);
});

test("dialog: author classes land on the dialog window's Css root", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /id: __dialog0Root/);
  assert.match(out, /cssClass: \["dlg"\]/);
});

test("dialog: children emit inside the dialog window's Css root", async () => {
  const out = await qml(DIALOG_SRC);
  const rootAt = out.indexOf("id: __dialog0Root");
  const bodyAt = out.indexOf('cssClass: ["body"]');
  assert.ok(rootAt >= 0 && bodyAt > rootAt, "author children must live inside the dialog root");
});

// ---------------------------------------------------------------------------
// <details> / <summary>
// ---------------------------------------------------------------------------

const DETAILS_SRC = `
  export function F() {
    return <details class="dt"><summary class="hd">More</summary><text class="line">body</text></details>;
  }
`;

test("details: wrapper CssFill with cssPrimitive 'details' and open cssState", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /cssPrimitive: "details"/);
  assert.match(out, /id: __details0/);
  assert.match(out, /cssState: __details0\.__open \? \["open"\] : \[\]/);
  assert.match(out, /property bool __open: !!\(false\)/);
});

test("details: open prop initializes __open", async () => {
  const out = await qml(`
    export function F() {
      const [o, setO] = createSignal(true);
      return <details open={o()}><summary>t</summary><text>c</text></details>;
    }
  `);
  assert.match(out, /property bool __open: !!\(o\)/);
});

test("details: summary renders as a header CssFill with toggle MouseArea", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /cssPrimitive: "summary"/);
  assert.match(out, /cssClass: \["hd"\]/);
  assert.match(out, /MouseArea \{/);
  assert.match(out, /onClicked: __details0\.__open = !__details0\.__open/);
  assert.match(out, /cssState: \(__details0\.__open \? \["open"\] : \[\]\)\.concat\(__hover0\.containsMouse \? \["hover"\] : \[\]\)/);
});

test("details: marker glyph is a plain Text inside an anchors.fill Item host", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /text: "▸"/);
  assert.match(out, /rotation: __details0\.__open \? 90 : 0/);
  assert.match(out, /Css\.CssItem \{ cssPrimitive: "text"; cssClass: \["marker"\] \}/);
  // Layout pitfall: the plain Text must live inside an anchored Item host.
  const itemAt = out.indexOf("Item {\n");
  const markerAt = out.indexOf('text: "▸"');
  assert.ok(itemAt >= 0 && itemAt < markerAt, "marker must be hosted in a plain Item");
  assert.match(out, /Item \{\n\s+anchors\.fill: parent\n\s+Text \{/);
});

test("details: remaining children render in a content CssRect visible only when open", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /cssClass: \["content"\]/);
  assert.match(out, /visible: __details0\.__open/);
  const contentAt = out.indexOf('cssClass: ["content"]');
  const lineAt = out.indexOf('cssClass: ["line"]');
  assert.ok(contentAt >= 0 && lineAt > contentAt, "body children must live inside the content box");
});

// ---------------------------------------------------------------------------
// counter threading — ids stay unique across mixed widgets
// ---------------------------------------------------------------------------

test("htmlwidgets: ids share the input counter with other widgets", async () => {
  const out = await qml(`
    export function F() {
      const [p, setP] = createSignal(0.5);
      return <div><input /><progress value={p()} /><details><summary>s</summary><text>c</text></details></div>;
    }
  `);
  assert.match(out, /id: __input0/);       // the text field
  assert.match(out, /__input1\.indeterminate/); // the progress control
  assert.match(out, /id: __details2/);     // details takes the next slot
});
