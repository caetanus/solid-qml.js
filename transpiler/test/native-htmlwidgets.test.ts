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

test("progress: the C++ Progress hosts the T.ProgressBar + bar internals as a snippet", async () => {
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/indicators.cpp", import.meta.url)), "utf8");
  assert.match(src, /T\.ProgressBar \{/);
  assert.match(src, /indeterminate: root\.value < 0/);
  assert.match(src, /cssClass: \["track"\]/);
  assert.match(src, /cssClass: \["bar"\]/);
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

test("fieldset: the C++ Fieldset is a CssFill fieldset box", async () => {
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/primitives.h", import.meta.url)), "utf8");
  assert.match(src, /class Fieldset : public QmlCss::CssFill/);
  assert.match(src, /QStringLiteral\("fieldset"\)/);
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

// Dialog is C++ now (widgets-to-cpp) — the Window snippet keeps the QML internals verbatim
// (`root` = the C++ page anchor, so wrap.* references became root.*).
const DIALOG_QML = await readFile(fileURLToPath(new URL("../../src/widgets/dialog.cpp", import.meta.url)), "utf8")
  + await readFile(fileURLToPath(new URL("../../src/widgets/dialog.h", import.meta.url)), "utf8");

test("dialog: <dialog> instantiates the W.Dialog component", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /W\.Dialog \{/);
  // The modal Window internals now live in Dialog.qml, not the emit.
  assert.doesNotMatch(out, /Window \{/);
  assert.doesNotMatch(out, /T\.Dialog/);
});

test("dialog: Dialog.qml is a REAL modal Window (Qt.Dialog), not an overlay popup", async () => {
  assert.match(DIALOG_QML, /Window \{/);
  assert.match(DIALOG_QML, /flags: Qt\.Dialog/);
  assert.match(DIALOG_QML, /modality: Qt\.WindowModal/);
  assert.match(DIALOG_QML, /transientParent: root\.pageWindow/);
  assert.match(DIALOG_QML, /cssPrimitive: "dialog"/);
  assert.doesNotMatch(DIALOG_QML, /T\.Dialog/);
  assert.doesNotMatch(DIALOG_QML, /T\.Overlay\.overlay/);
});

test("dialog: Dialog.qml window sizes itself to the content's implicit size", async () => {
  assert.match(DIALOG_QML, /width: Math\.max\(1, rootBox\.implicitWidth\)/);
  assert.match(DIALOG_QML, /height: Math\.max\(1, rootBox\.implicitHeight\)/);
});

test("dialog: open prop drives visible + a RestoreNone Binding (survives self-close)", async () => {
  const out = await qml(DIALOG_SRC);
  // The emit folds the open signal (and any Show guard) into the component's `open` prop.
  assert.match(out, /open: !!\(open\)/);
  // Dialog.qml drives the window visible from `open` via a plain binding AND a RestoreNone Binding.
  assert.match(DIALOG_QML, /visible: root\.open/);
  assert.match(DIALOG_QML, /Binding \{/);
  assert.match(DIALOG_QML, /target: dlg/);
  assert.match(DIALOG_QML, /property: "visible"/);
  assert.match(DIALOG_QML, /value: root\.open/);
  assert.match(DIALOG_QML, /restoreMode: Binding\.RestoreNone/);
});

test("dialog: onClose handler wires to onDialogClosed (relayed from the window's onClosing)", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /onDialogClosed: \{ open = false \}/);
  assert.match(DIALOG_QML, /onClosing: root\.dialogClosed\(\)/);
});

test("dialog: author classes land on the component cssClass (forwarded to the Css root)", async () => {
  const out = await qml(DIALOG_SRC);
  assert.match(out, /cssClass: \["dlg"\]/);
  assert.match(DIALOG_QML, /cssClass: root\.cssClass/);
});

test("dialog: Dialog.qml root re-anchors the CSS ancestor walk at the page wrapper", async () => {
  // A separate window severs the ancestor chain; cssAncestor restores scoped rules + inheritance.
  assert.match(DIALOG_QML, /property Item cssAncestor: root/);
});

test("dialog: children emit inside the W.Dialog instance (the default content slot)", async () => {
  const out = await qml(DIALOG_SRC);
  const dlgAt = out.indexOf("W.Dialog {");
  const bodyAt = out.indexOf('cssClass: ["body"]');
  assert.ok(dlgAt >= 0 && bodyAt > dlgAt, "author children must live inside the W.Dialog block");
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

test("details: the C++ Details holds the wrapper, marker and content internals", async () => {
  // Details is C++ now (widgets-to-cpp) — summary/body ride as snippets; `__open` follows the
  // `open` prop until the first user toggle (the init-binding semantics), then goes independent.
  const { readFile } = await import("node:fs/promises");
  const { fileURLToPath } = await import("node:url");
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/details.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/details.h", import.meta.url)), "utf8");
  assert.match(src, /setCssPrimitive\(QStringLiteral\("details"\)\)/);
  assert.match(src, /cssPrimitive: "summary"/);
  assert.match(src, /text: "▸"/);
  assert.match(src, /cssClass: \["marker"\]/);
  assert.match(src, /cssClass: \["content"\]/);
  assert.match(src, /Q_PROPERTY\(bool __open READ isOpenNow NOTIFY effectiveOpenChanged\)/);
  assert.match(src, /if \(!m_userToggled\)\s*\n\s*setEffectiveOpen\(v\)/);
  assert.match(src, /Q_PROPERTY\(QQmlListProperty<QObject> summaryContent READ summaryContent CONSTANT\)/);
  assert.match(src, /Q_CLASSINFO\("DefaultProperty", "content"\)/);
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
