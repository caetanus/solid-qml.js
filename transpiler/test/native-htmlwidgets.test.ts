/** Native HTML-widget group tests: <progress>, <fieldset>/<legend>, <dialog>, <details>/<summary>.
 *
 *  Pattern mirrors test/widgets.test.ts: a local `qml()` helper for direct emitQml calls,
 *  and `qmlType()` for full emitComponentType output (needed for the Templates import check). */

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
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

test("progress: <progress> instantiates the W.Progress component", async () => {
  const out = await qml(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /W\.Progress \{/);
  // The T.ProgressBar/track/bar internals now live in Progress.qml, not the emit.
  assert.doesNotMatch(out, /T\.ProgressBar/);
});

test("progress: class lands on the component cssClass", async () => {
  const out = await qml(`export function F(){ return <progress class="hw-progress" value={0.5} />; }`);
  assert.match(out, /cssClass: \["hw-progress"\]/);
});

// ---------------------------------------------------------------------------
// <progress> — prop wiring
// ---------------------------------------------------------------------------

test("progress: value={sig()} binds value; no value → indeterminate (component default -1)", async () => {
  const out = await qml(`
    export function F() {
      const [pct, setPct] = createSignal(0.25);
      return <progress value={pct()} />;
    }
  `);
  assert.match(out, /value: pct/);

  const noVal = await qml(`export function F(){ return <progress />; }`);
  assert.doesNotMatch(noVal, /\bvalue:/); // omitted → component's -1 default = indeterminate
});

test("progress: max={100} maps to the component's max prop; default omitted (HTML spec = 1)", async () => {
  const out = await qml(`export function F(){ return <progress max={100} value={40} />; }`);
  assert.match(out, /max: 100/);
  const dflt = await qml(`export function F(){ return <progress value={0.5} />; }`);
  assert.doesNotMatch(dflt, /\bmax:/); // omitted → component default (1)
});

test("progress: emitting the widget imports the solidqml.Widgets module", async () => {
  const out = await qmlType(`export function F(){ return <progress value={0.5} />; }`);
  assert.match(out, /import solidqml\.Widgets .* as W/);
});

test("progress: Progress.qml component exists with the T.ProgressBar + bar internals", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Progress.qml", import.meta.url)), "utf8");
  assert.match(src, /T\.ProgressBar/);
  assert.match(src, /cssClass: \["track"\]/);
  assert.match(src, /cssClass: \["bar"\]/);
  assert.match(src, /indeterminate: root\.value < 0/);
});

// ---------------------------------------------------------------------------
// <fieldset> / <legend>
// ---------------------------------------------------------------------------

test("fieldset: <fieldset> instantiates the W.Fieldset component and renders children", async () => {
  const out = await qml(`export function F(){ return <fieldset class="fs"><text class="a">hi</text></fieldset>; }`);
  assert.match(out, /W\.Fieldset \{/);
  // The cssPrimitive "fieldset" wrapper now lives in Fieldset.qml, not the emit.
  assert.doesNotMatch(out, /cssPrimitive: "fieldset"/);
  assert.match(out, /cssClass: \["fs"\]/);
  assert.match(out, /cssClass: \["a"\]/);
});

test("fieldset: Fieldset.qml component exists as a CssFill fieldset box", async () => {
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Fieldset.qml", import.meta.url)), "utf8");
  assert.match(src, /Css\.CssFill \{/);
  assert.match(src, /cssPrimitive: "fieldset"/);
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

test("fieldset: emitting the widget imports the solidqml.Widgets module", async () => {
  const out = await qmlType(`export function F(){ return <fieldset><legend>L</legend></fieldset>; }`);
  assert.match(out, /import solidqml\.Widgets .* as W/);
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

test("dialog: root re-anchors the CSS ancestor walk at the page wrapper", async () => {
  const out = await qml(DIALOG_SRC);
  // A separate window severs the ancestor chain; cssAncestor restores scoped rules + inheritance.
  assert.match(out, /property Item cssAncestor: __dialog0W/);
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

test("details: <details> instantiates the W.Details component (no inline __open/wrapper)", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /W\.Details \{/);
  assert.match(out, /cssClass: \["dt"\]/);
  // The CssFill "details" wrapper, __open and the summary/content boxes now live in Details.qml.
  assert.doesNotMatch(out, /cssPrimitive: "details"/);
  assert.doesNotMatch(out, /property bool __open/);
  assert.doesNotMatch(out, /id: __details/);
});

test("details: no open prop → omitted (component default false)", async () => {
  const out = await qml(DETAILS_SRC);
  assert.doesNotMatch(out, /\bopen:/);
});

test("details: open prop seeds the component's open (init binding source)", async () => {
  const out = await qml(`
    export function F() {
      const [o, setO] = createSignal(true);
      return <details open={o()}><summary>t</summary><text>c</text></details>;
    }
  `);
  assert.match(out, /open: o/);
});

test("details: summary author classes pass through as summaryClass", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /summaryClass: \["hd"\]/);
});

test("details: summary children pass through as a summaryContent list literal", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /summaryContent: \[/);
  const sumAt = out.indexOf("summaryContent: [");
  const moreAt = out.indexOf('text: "More"');
  assert.ok(sumAt >= 0 && moreAt > sumAt, "summary label lives inside the summaryContent list");
  // The marker glyph / MouseArea now live in Details.qml, not the emit.
  assert.doesNotMatch(out, /text: "▸"/);
});

test("details: disclosure body passes as the default content children", async () => {
  const out = await qml(DETAILS_SRC);
  assert.match(out, /cssClass: \["line"\]/);
  // The content CssRect (cssClass ["content"], visible: __open) is inside Details.qml.
  assert.doesNotMatch(out, /cssClass: \["content"\]/);
});

test("details: Details.qml component holds the wrapper, marker and content internals", async () => {
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Details.qml", import.meta.url)), "utf8");
  assert.match(src, /cssPrimitive: "details"/);
  assert.match(src, /cssPrimitive: "summary"/);
  assert.match(src, /text: "▸"/);
  assert.match(src, /cssClass: \["marker"\]/);
  assert.match(src, /cssClass: \["content"\]/);
  assert.match(src, /property bool __open: !!\(root\.open\)/);
  assert.match(src, /property alias summaryContent: summaryBox\.data/);
  assert.match(src, /default property alias content: contentBox\.data/);
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
  assert.match(out, /W\.Progress \{/);     // progress is a component instance (no counter id)
  assert.match(out, /W\.Details \{/);      // details is a component instance (no counter id)
  // Neither progress nor details consumes an input-counter slot now; only the text field does.
  assert.doesNotMatch(out, /id: __input1/);
  assert.doesNotMatch(out, /id: __details/);
});
