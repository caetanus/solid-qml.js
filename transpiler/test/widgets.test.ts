/** Phase 2 widget tests: <input> and <textarea> → T.TextField / T.TextArea.
 *
 *  Pattern mirrors test/qml.test.ts: a local `qml()` helper for direct emitQml calls,
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
// <input> — wrapper shape and basic properties
// ---------------------------------------------------------------------------

test("widgets: <input> emits wrapper CssFill with cssPrimitive 'input'", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
});

test("widgets: <input> wrapper carries cssState for focus and disabled", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: <input> wrapper mirrors implicitWidth/Height from the control", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /implicitWidth: __input0\.implicitWidth/);
  assert.match(out, /implicitHeight: __input0\.implicitHeight/);
});

test("widgets: <input> emits T.TextField inside the wrapper with background null", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /T\.TextField \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
  assert.match(out, /background: null/);
});

test("widgets: <input> bridges CSS colour and font from parent inherited properties", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /color: cssTheme\.parseColor\(parent\.inheritedColor \|\| "#2b2b2b"\)/);
  assert.match(out, /font\.family: cssTheme\.resolveFontFamily\(parent\.inheritedFontFamily/);
  assert.match(out, /font\.pixelSize: cssTheme\.parseFontSize\(parent\.inheritedFontSize/);
});

test("widgets: <input class='x'> passes the class to the wrapper cssClass", async () => {
  const out = await qml(`export function F(){ return <input class="x" />; }`);
  assert.match(out, /cssClass: \["x"\]/);
});

// ---------------------------------------------------------------------------
// password echoMode
// ---------------------------------------------------------------------------

test("widgets: <input type='password'> adds echoMode: TextInput.Password", async () => {
  const out = await qml(`export function F(){ return <input type="password" />; }`);
  assert.match(out, /echoMode: TextInput\.Password/);
});

test("widgets: <input type='text'> does NOT emit echoMode", async () => {
  const out = await qml(`export function F(){ return <input type="text" />; }`);
  assert.doesNotMatch(out, /echoMode/);
});

test("widgets: <input type='email'> does NOT emit echoMode", async () => {
  const out = await qml(`export function F(){ return <input type="email" />; }`);
  assert.doesNotMatch(out, /echoMode/);
});

// ---------------------------------------------------------------------------
// Controlled value: Binding element
// ---------------------------------------------------------------------------

test("widgets: value={sig()} emits a Binding element targeting the control", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <input value={v()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "text"/);
  assert.match(out, /value: v/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: <input> without value does NOT emit a Binding element", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

// ---------------------------------------------------------------------------
// onInput handler → onTextEdited
// ---------------------------------------------------------------------------

test("widgets: onInput={(e) => set(e.target.value)} wires onTextEdited with text", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <input value={v()} onInput={(e) => setV(e.target.value)} />;
    }
  `);
  // e.target.value is translated to bare `text` (the control's text in the handler scope).
  assert.match(out, /onTextEdited: \{ v = text \}/);
});

test("widgets: onInput with e.currentTarget.value also translates to text", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <input value={v()} onInput={(e) => setV(e.currentTarget.value)} />;
    }
  `);
  assert.match(out, /onTextEdited: \{ v = text \}/);
});

// ---------------------------------------------------------------------------
// onChange handler → onEditingFinished
// ---------------------------------------------------------------------------

test("widgets: onChange wires onEditingFinished on T.TextField", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <input value={v()} onChange={(e) => setV(e.target.value)} />;
    }
  `);
  assert.match(out, /onEditingFinished: \{ v = text \}/);
});

// ---------------------------------------------------------------------------
// disabled → enabled: false + cssState contains disabled
// ---------------------------------------------------------------------------

test("widgets: disabled prop sets enabled: false on the control", async () => {
  const out = await qml(`export function F(){ return <input disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: disabled input still has disabled in cssState expression", async () => {
  const out = await qml(`export function F(){ return <input disabled />; }`);
  // The cssState expression always carries the disabled concat (runtime-evaluated from enabled).
  assert.match(out, /!__input0\.enabled \? \["disabled"\] : \[\]/);
});

// ---------------------------------------------------------------------------
// readonly → readOnly: true
// ---------------------------------------------------------------------------

test("widgets: readonly prop sets readOnly: true on the control", async () => {
  const out = await qml(`export function F(){ return <input readonly />; }`);
  assert.match(out, /readOnly: true/);
});

// ---------------------------------------------------------------------------
// maxlength → maximumLength
// ---------------------------------------------------------------------------

test("widgets: maxlength maps to maximumLength on T.TextField", async () => {
  const out = await qml(`export function F(){ return <input maxlength={100} />; }`);
  assert.match(out, /maximumLength: 100/);
});

// ---------------------------------------------------------------------------
// placeholder
// ---------------------------------------------------------------------------

test("widgets: placeholder emits an overlay Text inside T.TextField", async () => {
  const out = await qml(`export function F(){ return <input placeholder="Type here" />; }`);
  // The overlay lives inside T.TextField (level 2) and hides on focus or when text is present.
  assert.match(out, /text: "Type here"/);
  assert.match(out, /visible: parent\.text\.length === 0 && !parent\.activeFocus/);
});

test("widgets: <input> without placeholder does NOT emit the overlay Text", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  // No placeholder attribute → no placeholder Text child emitted.
  const textCount = (out.match(/Text \{/g) ?? []).length;
  assert.equal(textCount, 0);
});

// ---------------------------------------------------------------------------
// <textarea>
// ---------------------------------------------------------------------------

test("widgets: <textarea> emits CssFill with cssPrimitive 'textarea'", async () => {
  const out = await qml(`export function F(){ return <textarea></textarea>; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "textarea"/);
});

test("widgets: <textarea> inner control is T.TextArea with wrapMode and background null", async () => {
  const out = await qml(`export function F(){ return <textarea></textarea>; }`);
  assert.match(out, /T\.TextArea \{/);
  assert.match(out, /wrapMode: TextEdit\.Wrap/);
  assert.match(out, /background: null/);
});

test("widgets: <textarea> cssState includes focus and disabled (same pattern as input)", async () => {
  const out = await qml(`export function F(){ return <textarea></textarea>; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)/);
});

test("widgets: <textarea> onInput maps to onTextChanged (T.TextArea has no textEdited)", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <textarea value={v()} onInput={(e) => setV(e.target.value)}></textarea>;
    }
  `);
  assert.match(out, /onTextChanged: \{ v = text \}/);
  assert.doesNotMatch(out, /onTextEdited/);
});

test("widgets: <textarea> value={sig()} emits a Binding element", async () => {
  const out = await qmlType(`
    export function F() {
      const [v, setV] = createSignal("");
      return <textarea value={v()}></textarea>;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "text"/);
  assert.match(out, /value: v/);
});

test("widgets: <textarea disabled> sets enabled: false", async () => {
  const out = await qml(`export function F(){ return <textarea disabled></textarea>; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: <textarea placeholder='...'>  emits an overlay Text anchored top-left", async () => {
  const out = await qml(`export function F(){ return <textarea placeholder="Notes..."></textarea>; }`);
  assert.match(out, /text: "Notes\.\.\."/);
  assert.match(out, /anchors\.top: parent\.top/);
});

// ---------------------------------------------------------------------------
// Multiple widgets in the same component — id counter increments
// ---------------------------------------------------------------------------

test("widgets: two <input>s in the same component get distinct ids (__input0, __input1)", async () => {
  const out = await qml(`
    export function F(){
      return <div><input /><input /></div>;
    }
  `);
  assert.match(out, /id: __input0/);
  assert.match(out, /id: __input1/);
});

// ---------------------------------------------------------------------------
// Templates import — appears only when a widget is emitted
// ---------------------------------------------------------------------------

test("widgets: Templates import is prepended when component contains an <input>", async () => {
  const out = await qmlType(`
    export function F() {
      return <input />;
    }
  `);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

test("widgets: Templates import is prepended when component contains a <textarea>", async () => {
  const out = await qmlType(`
    export function F() {
      return <textarea></textarea>;
    }
  `);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

test("widgets: Templates import is NOT emitted for a plain div (no widget)", async () => {
  const out = await qmlType(`
    export function F() {
      return <div class="box"></div>;
    }
  `);
  assert.doesNotMatch(out, /QtQuick\.Templates/);
});

test("widgets: Templates import is NOT emitted for a button (no widget)", async () => {
  const out = await qmlType(`
    export function F() {
      const [x, setX] = createSignal(0);
      return <button onClick={() => setX(x() + 1)}>click</button>;
    }
  `);
  assert.doesNotMatch(out, /QtQuick\.Templates/);
});

// ---------------------------------------------------------------------------
// Phase 3: <input type="checkbox"> — checkbox toggle
// ---------------------------------------------------------------------------

test("widgets: <input type='checkbox'> emits wrapper CssFill + T.CheckBox inside", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
  assert.match(out, /T\.CheckBox \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
  assert.match(out, /background: null/);
  assert.match(out, /contentItem: null/);
});

test("widgets: checkbox wrapper cssState carries 'checked' and 'disabled' pseudo-classes", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  // Both wrapper and indicator carry the same checked/disabled expression.
  assert.match(out, /__input0\.checked \? \["checked"\] : \[\]/);
  assert.match(out, /!__input0\.enabled \? \["disabled"\] : \[\]/);
});

test("widgets: checkbox indicator CssFill has explicit width/height 20 + implicitWidth/Height 20", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /cssClass: \["indicator"\]/);
  // All four size props hardcoded — CSS geometry won't apply (indicator is inside T.CheckBox,
  // not a Css container), but author can still override via CSS colour/background rules.
  assert.match(out, /width: 20/);
  assert.match(out, /height: 20/);
  assert.match(out, /implicitWidth: 20/);
  assert.match(out, /implicitHeight: 20/);
});

test("widgets: checkbox indicator contains a CssText glyph visible when checked", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /cssClass: \["indicator-glyph"\]/);
  assert.match(out, /text: "✓"/);
  assert.match(out, /visible: __input0\.checked/);
  assert.match(out, /anchors\.centerIn: parent/);
});

test("widgets: checkbox checked={sig()} emits Binding on property 'checked'", async () => {
  const out = await qmlType(`
    export function F() {
      const [isOn, setIsOn] = createSignal(false);
      return <input type="checkbox" checked={isOn()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "checked"/);
  assert.match(out, /value: isOn/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: checkbox onChange wires onToggled with e.target.checked translated to ctl.checked", async () => {
  const out = await qmlType(`
    export function F() {
      const [isOn, setIsOn] = createSignal(false);
      return <input type="checkbox" checked={isOn()} onChange={(e) => setIsOn(e.target.checked)} />;
    }
  `);
  assert.match(out, /onToggled: \{ isOn = __input0\.checked \}/);
});

test("widgets: checkbox disabled prop sets enabled: false on T.CheckBox", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: checkbox without checked does NOT emit a Binding", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

test("widgets: checkbox Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

// ---------------------------------------------------------------------------
// Phase 3: <input type="checkbox" role="switch"> — switch toggle
// ---------------------------------------------------------------------------

test("widgets: switch emits T.Switch + track CssFill (cssClass 'track') + knob CssRect", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /T\.Switch \{/);
  assert.match(out, /cssClass: \["track"\]/);
  assert.match(out, /cssClass: \["knob"\]/);
});

test("widgets: switch track is 36x20 and knob is 16x16 (hardcoded — not in Css container)", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /width: 36/);
  assert.match(out, /height: 20/);
  assert.match(out, /implicitWidth: 36/);
  assert.match(out, /implicitHeight: 20/);
  assert.match(out, /width: 16/);
  assert.match(out, /height: 16/);
});

test("widgets: switch knob x uses visualPosition binding for animated slide", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /x: __input0\.visualPosition \* \(parent\.width - width\)/);
});

test("widgets: switch knob has Behavior on x with NumberAnimation 120ms", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /Behavior on x \{ NumberAnimation \{ duration: 120 \} \}/);
});

test("widgets: switch checked={sig()} emits Binding on property 'checked'", async () => {
  const out = await qmlType(`
    export function F() {
      const [sw, setSw] = createSignal(false);
      return <input type="checkbox" role="switch" checked={sw()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /property: "checked"/);
  assert.match(out, /value: sw/);
});

test("widgets: switch onChange wires onToggled with translated handler", async () => {
  const out = await qmlType(`
    export function F() {
      const [sw, setSw] = createSignal(false);
      return <input type="checkbox" role="switch" checked={sw()} onChange={(e) => setSw(e.target.checked)} />;
    }
  `);
  assert.match(out, /onToggled: \{ sw = __input0\.checked \}/);
});

test("widgets: switch wrapper cssState carries checked/disabled", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /cssState: \(__input0\.checked \? \["checked"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

// ---------------------------------------------------------------------------
// Phase 3: <input type="radio" name="…"> — radio button + ButtonGroup
// ---------------------------------------------------------------------------

test("widgets: <input type='radio'> emits T.RadioButton + indicator CssFill + indicator-dot", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /T\.RadioButton \{/);
  assert.match(out, /cssClass: \["indicator"\]/);
  assert.match(out, /cssClass: \["indicator-dot"\]/);
});

test("widgets: radio indicator-dot is visible when checked and centred", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /visible: __input0\.checked/);
  assert.match(out, /anchors\.centerIn: parent/);
  // Dot size: 8x8
  assert.match(out, /width: 8/);
  assert.match(out, /height: 8/);
});

test("widgets: radio references its ButtonGroup via T.ButtonGroup.group", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="a" />; }`);
  assert.match(out, /T\.ButtonGroup\.group: __group_a/);
});

test("widgets: two radios with name='a' share ONE T.ButtonGroup declaration", async () => {
  const out = await qmlType(`
    export function F() {
      return <div><input type="radio" name="a" /><input type="radio" name="a" /></div>;
    }
  `);
  // Only one ButtonGroup for name "a"
  const groupDeclCount = (out.match(/T\.ButtonGroup \{ id: __group_a \}/g) ?? []).length;
  assert.equal(groupDeclCount, 1, "exactly one T.ButtonGroup declaration for name='a'");
  // Both radios reference the same group
  const groupRefCount = (out.match(/T\.ButtonGroup\.group: __group_a/g) ?? []).length;
  assert.equal(groupRefCount, 2, "both radios reference __group_a");
});

test("widgets: two radios with different names get separate T.ButtonGroup declarations", async () => {
  const out = await qmlType(`
    export function F() {
      return <div>
        <input type="radio" name="a" />
        <input type="radio" name="b" />
      </div>;
    }
  `);
  assert.match(out, /T\.ButtonGroup \{ id: __group_a \}/);
  assert.match(out, /T\.ButtonGroup \{ id: __group_b \}/);
  const totalGroups = (out.match(/T\.ButtonGroup \{/g) ?? []).length;
  assert.equal(totalGroups, 2, "two distinct ButtonGroup declarations for two names");
});

test("widgets: radio checked={sig()} emits Binding on property 'checked'", async () => {
  const out = await qmlType(`
    export function F() {
      const [sel, setSel] = createSignal(false);
      return <input type="radio" name="x" checked={sel()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /property: "checked"/);
  assert.match(out, /value: sel/);
});

test("widgets: radio onChange wires onToggled with e.target.checked translated", async () => {
  const out = await qmlType(`
    export function F() {
      const [sel, setSel] = createSignal(false);
      return <input type="radio" name="x" checked={sel()} onChange={(e) => setSel(e.target.checked)} />;
    }
  `);
  assert.match(out, /onToggled: \{ sel = __input0\.checked \}/);
});

test("widgets: radio Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="radio" name="x" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});
