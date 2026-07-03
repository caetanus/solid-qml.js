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

test("widgets: switch wrapper cssState carries focus/checked/disabled", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(__input0\.checked \? \["checked"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

// ---------------------------------------------------------------------------
// Phase 3: <input type="radio" name="…"> — radio button + ButtonGroup
// ---------------------------------------------------------------------------

test("widgets: <input type='radio'> emits T.RadioButton + indicator CssFill + indicator-dot", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /T\.RadioButton \{/);
  assert.match(out, /cssClass: \["indicator"\]/);
  assert.match(out, /cssClass: __input0\.checked \? \["indicator-dot", "checked"\] : \["indicator-dot"\]/);
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

// ---------------------------------------------------------------------------
// Phase 4: <select> / <option> → T.ComboBox
// ---------------------------------------------------------------------------

test("widgets: <select> emits wrapper CssFill with cssPrimitive 'select'", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "select"/);
});

test("widgets: <select> emits T.ComboBox inside with background null and leftPadding 12", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /T\.ComboBox \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
  assert.match(out, /background: null/);
  assert.match(out, /leftPadding: 12/);
});

test("widgets: <select> model array contains option labels in order", async () => {
  const out = await qml(`export function F(){ return (
    <select><option value="a">Alpha</option><option value="b">Beta</option></select>
  ); }`);
  assert.match(out, /model: \["Alpha", "Beta"\]/);
});

test("widgets: <select> readonly __values property holds option values", async () => {
  const out = await qml(`export function F(){ return (
    <select><option value="a">Alpha</option><option value="b">Beta</option></select>
  ); }`);
  assert.match(out, /readonly property var __values: \["a", "b"\]/);
});

test("widgets: <option> without value attr uses the label as value", async () => {
  const out = await qml(`export function F(){ return (
    <select><option>Baz</option></select>
  ); }`);
  // Both model label and __values entry should be "Baz"
  assert.match(out, /model: \["Baz"\]/);
  assert.match(out, /__values: \["Baz"\]/);
});

test("widgets: <select> contentItem is CssText with cssClass ['value'] showing displayText", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /contentItem: Css\.CssText \{/);
  assert.match(out, /cssClass: \["value"\]/);
  assert.match(out, /text: __input0\.displayText/);
});

test("widgets: <select> emits a chevron CssText with cssClass ['chevron'] anchored right", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /cssClass: \["chevron"\]/);
  assert.match(out, /text: "▾"/);
  assert.match(out, /anchors\.right: parent\.right/);
  assert.match(out, /anchors\.verticalCenter: parent\.verticalCenter/);
});

test("widgets: <select> delegate is T.ItemDelegate with explicit width from popup.width", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /delegate: T\.ItemDelegate \{/);
  assert.match(out, /width: __input0\.popup\.width/);
  assert.match(out, /implicitHeight: 36/);
});

test("widgets: <select> delegate background CssFill has cssClass ['option'] and hover/selected cssState", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /cssClass: \["option"\]/);
  // hover: optDelegate.highlighted; selected: currentIndex === index
  assert.match(out, /__optDel0\.highlighted \? \["hover"\] : \[\]/);
  assert.match(out, /__input0\.currentIndex === index \? \["selected"\] : \[\]/);
});

test("widgets: <select> delegate contentItem is CssText with cssClass ['option-label']", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /cssClass: \["option-label"\]/);
  assert.match(out, /text: modelData/);
});

test("widgets: <select> popup is T.Popup with y below (flip expr), width = combo.width, padding 1", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /popup: T\.Popup \{/);
  assert.match(out, /: __input0\.height \+ 2/); // flip expression ends in the below-position
  assert.match(out, /width: __input0\.width/);
  assert.match(out, /padding: 1/);
});

test("widgets: <select> popup background is CssFill cssClass ['popup']", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /cssClass: \["popup"\]/);
});

test("widgets: <select> popup contentItem is ListView with delegateModel and capped implicitHeight", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /contentItem: ListView \{/);
  assert.match(out, /model: __input0\.delegateModel/);
  assert.match(out, /currentIndex: __input0\.highlightedIndex/);
  assert.match(out, /implicitHeight: Math\.min\(contentHeight, 240\)/);
});

test("widgets: <select> cssState carries focus and disabled on the wrapper", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /__input0\.activeFocus \? \["focus"\] : \[\]/);
  assert.match(out, /!__input0\.enabled \? \["disabled"\] : \[\]/);
});

test("widgets: <select> value={sig()} emits Binding on currentIndex via indexOf", async () => {
  const out = await qmlType(`
    export function F() {
      const [sel, setSel] = createSignal("a");
      return (
        <select value={sel()}>
          <option value="a">Alpha</option>
          <option value="b">Beta</option>
        </select>
      );
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "currentIndex"/);
  assert.match(out, /value: __input0\.__values\.indexOf\(sel\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: <select> without value does NOT emit a Binding", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

test("widgets: <select> onChange wires onActivated with e.target.value → __values[index]", async () => {
  const out = await qmlType(`
    export function F() {
      const [sel, setSel] = createSignal("a");
      return (
        <select value={sel()} onChange={(e) => setSel(e.target.value)}>
          <option value="a">Alpha</option>
        </select>
      );
    }
  `);
  assert.match(out, /onActivated: \(index\) => \{ sel = __input0\.__values\[index\] \}/);
});

test("widgets: <select> without onChange does NOT emit onActivated", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.doesNotMatch(out, /onActivated/);
});

test("widgets: <select> disabled prop sets enabled: false on T.ComboBox", async () => {
  const out = await qml(`export function F(){ return <select disabled><option>A</option></select>; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: <select> Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /import QtQuick\.Templates 6\.8 as T/);
});

test("widgets: dynamic <option> content throws a clear transpiler error", async () => {
  await assert.rejects(
    qml(`export function F(){ return <select><option>{someExpr}</option></select>; }`),
    /dynamic.*option.*not supported/i,
  );
});

// ---------------------------------------------------------------------------
// Phase 4: <input type="range"> → T.Slider
// ---------------------------------------------------------------------------

test("widgets: <input type='range'> emits wrapper CssFill with cssPrimitive 'input'", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
});

test("widgets: <input type='range'> emits T.Slider anchors.fill:parent inside the wrapper", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /T\.Slider \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
});

test("widgets: <input type='range'> defaults from=0 to=100 stepSize=1", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 100/);
  assert.match(out, /stepSize: 1/);
});

test("widgets: <input type='range' min=5 max=50 step=5> overrides range and step", async () => {
  const out = await qml(`export function F(){ return <input type="range" min={5} max={50} step={5} />; }`);
  assert.match(out, /from: 5/);
  assert.match(out, /to: 50/);
  assert.match(out, /stepSize: 5/);
});

test("widgets: <input type='range'> background is CssFill cssClass ['track'] with height 6", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /background: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["track"\]/);
  assert.match(out, /height: 6/);
  assert.match(out, /implicitHeight: 6/);
});

test("widgets: <input type='range'> track background uses Qt-Basic-style x/y/width geometry", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /x: __input0\.leftPadding/);
  assert.match(out, /y: __input0\.topPadding \+ \(__input0\.availableHeight - height\) \/ 2/);
  assert.match(out, /width: __input0\.availableWidth/);
});

test("widgets: <input type='range'> track contains CssRect cssClass ['track-fill'] width driven by visualPosition", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /cssClass: \["track-fill"\]/);
  assert.match(out, /width: __input0\.visualPosition \* parent\.width/);
});

test("widgets: <input type='range'> handle is CssRect cssClass ['handle'] 18x18", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /handle: Css\.CssRect \{/);
  assert.match(out, /cssClass: \["handle"\]/);
  assert.match(out, /width: 18/);
  assert.match(out, /height: 18/);
  assert.match(out, /implicitWidth: 18/);
  assert.match(out, /implicitHeight: 18/);
});

test("widgets: <input type='range'> handle x/y use visualPosition and availableWidth/Height", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /x: __input0\.leftPadding \+ __input0\.visualPosition \* \(__input0\.availableWidth - width\)/);
  assert.match(out, /y: __input0\.topPadding \+ __input0\.availableHeight \/ 2 - height \/ 2/);
});

test("widgets: range value={sig()} emits Binding on property 'value'", async () => {
  const out = await qmlType(`
    export function F() {
      const [rv, setRv] = createSignal(50);
      return <input type="range" value={rv()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "value"/);
  assert.match(out, /value: rv/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: range without value does NOT emit a Binding", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

test("widgets: range onInput wires onMoved with e.target.value → ctl.value", async () => {
  const out = await qmlType(`
    export function F() {
      const [rv, setRv] = createSignal(0);
      return <input type="range" value={rv()} onInput={(e) => setRv(e.target.value)} />;
    }
  `);
  assert.match(out, /onMoved: \{ rv = __input0\.value \}/);
});

test("widgets: range onChange also wires to onMoved (approximation of HTML commit-on-release)", async () => {
  const out = await qmlType(`
    export function F() {
      const [rv, setRv] = createSignal(0);
      return <input type="range" value={rv()} onChange={(e) => setRv(e.target.value)} />;
    }
  `);
  assert.match(out, /onMoved: \{ rv = __input0\.value \}/);
});

test("widgets: range disabled sets enabled: false on T.Slider", async () => {
  const out = await qml(`export function F(){ return <input type="range" disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: range cssState carries focus and disabled", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: range Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

// ---------------------------------------------------------------------------
// Phase 4: <input type="number"> → T.SpinBox
// ---------------------------------------------------------------------------

test("widgets: <input type='number'> emits wrapper CssFill with cssPrimitive 'input'", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
});

test("widgets: <input type='number'> emits T.SpinBox with editable:true and background:null", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /T\.SpinBox \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /anchors\.fill: parent/);
  assert.match(out, /background: null/);
  assert.match(out, /editable: true/);
});

test("widgets: <input type='number'> defaults from=0 to=100 stepSize=1", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 100/);
  assert.match(out, /stepSize: 1/);
});

test("widgets: <input type='number' min=1 max=10 step=2> overrides the range", async () => {
  const out = await qml(`export function F(){ return <input type="number" min={1} max={10} step={2} />; }`);
  assert.match(out, /from: 1/);
  assert.match(out, /to: 10/);
  assert.match(out, /stepSize: 2/);
});

test("widgets: <input type='number'> contentItem is TextInput with displayText and validator", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /contentItem: TextInput \{/);
  assert.match(out, /text: __input0\.displayText/);
  assert.match(out, /validator: __input0\.validator/);
  assert.match(out, /readOnly: !__input0\.editable/);
});

test("widgets: <input type='number'> contentItem bridges color/font via ctlId.parent (CssFill wrapper)", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  // color and font come from the CssFill wrapper via __inputN.parent.inheritedX
  assert.match(out, /color: cssTheme\.parseColor\(__input0\.parent\.inheritedColor \|\| "#2b2b2b"\)/);
  assert.match(out, /font\.family: cssTheme\.resolveFontFamily\(__input0\.parent\.inheritedFontFamily/);
  assert.match(out, /font\.pixelSize: cssTheme\.parseFontSize\(__input0\.parent\.inheritedFontSize/);
});

test("widgets: <input type='number'> up.indicator is CssFill cssClass ['spin-up'] at top-right", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /up\.indicator: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["spin-up"\]/);
  // 2px inset keeps the buttons inside the wrapper's rounded border.
  assert.match(out, /x: parent\.width - width - 2/);
  assert.match(out, /y: 2/);
  assert.match(out, /width: 24/);
});

test("widgets: <input type='number'> down.indicator is CssFill cssClass ['spin-down'] at bottom-right", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /down\.indicator: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["spin-down"\]/);
  assert.match(out, /y: parent\.height \/ 2/);
});

test("widgets: <input type='number'> indicators contain CssText '+' and '−' glyphs centred", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /cssClass: \["spin-glyph"\]/);
  assert.match(out, /text: "\+"/);
  assert.match(out, /text: "−"/);
  assert.match(out, /anchors\.centerIn: parent/);
});

test("widgets: <input type='number'> up/down indicators cssState carries 'active' when pressed", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /__input0\.up\.pressed \? \["active"\] : \[\]/);
  assert.match(out, /__input0\.down\.pressed \? \["active"\] : \[\]/);
});

test("widgets: number value={sig()} emits Binding on property 'value'", async () => {
  const out = await qmlType(`
    export function F() {
      const [nv, setNv] = createSignal(5);
      return <input type="number" value={nv()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "value"/);
  assert.match(out, /value: nv/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: number without value does NOT emit a Binding", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

test("widgets: number onChange wires onValueModified with e.target.value → ctl.value", async () => {
  const out = await qmlType(`
    export function F() {
      const [nv, setNv] = createSignal(0);
      return <input type="number" value={nv()} onChange={(e) => setNv(e.target.value)} />;
    }
  `);
  assert.match(out, /onValueModified: \{ nv = __input0\.value \}/);
});

test("widgets: number disabled sets enabled: false on T.SpinBox", async () => {
  const out = await qml(`export function F(){ return <input type="number" disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: number cssState carries focus and disabled", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: number Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

// ---------------------------------------------------------------------------
// Phase 5: <Calendar> — inline month grid
// ---------------------------------------------------------------------------

test("widgets: <Calendar> emits wrapper CssFill with cssPrimitive 'div'", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "div"/);
});

test("widgets: <Calendar> emits T.AbstractMonthGrid with month and year bound to view properties", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /T\.AbstractMonthGrid \{/);
  assert.match(out, /month: __cal0\.__calMonth0/);
  assert.match(out, /year: __cal0\.__calYear0/);
});

test("widgets: <Calendar> T.AbstractMonthGrid carries height: parent.height - 56 so rows are visible", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  // Without explicit height, T.AbstractMonthGrid collapses to 0 and no day cells render.
  assert.match(out, /height: parent\.height - 56/);
});

test("widgets: <Calendar> emits T.AbstractDayOfWeekRow with Css.CssText delegate", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /T\.AbstractDayOfWeekRow \{/);
  assert.match(out, /cssClass: \["dow"\]/);
  assert.match(out, /text: model\.shortName/);
});

test("widgets: <Calendar> emits prev/next cal-nav buttons and cal-title label", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /cssClass: \["cal-nav"\]/);
  assert.match(out, /cssClass: \["cal-title"\]/);
  assert.match(out, /text: "‹"/);
  assert.match(out, /text: "›"/);
});

test("widgets: <Calendar> nav prev button wraps Dec→Jan on month 0", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  // Month wrap: if month==0, year-- and month=11
  assert.match(out, /__cal0\.__calMonth0 === 0/);
  assert.match(out, /__cal0\.__calYear0 = __cal0\.__calYear0 - 1/);
  assert.match(out, /__cal0\.__calMonth0 = 11/);
});

test("widgets: <Calendar> nav next button wraps Dec→Jan on month 11", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /__cal0\.__calMonth0 === 11/);
  assert.match(out, /__cal0\.__calYear0 = __cal0\.__calYear0 \+ 1/);
  assert.match(out, /__cal0\.__calMonth0 = 0/);
});

test("widgets: <Calendar> day delegate carries cssState: today/selected/outside/hover", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /cssClass: \["day"\]/);
  assert.match(out, /model\.today \? \["today"\]/);
  assert.match(out, /\["selected"\]/);
  assert.match(out, /model\.month !== __mg0\.month \? \["outside"\]/);
  assert.match(out, /__mgDel0\.hovered \? \["hover"\]/);
});

test("widgets: <Calendar> day delegate contentItem is Css.CssText with class 'day-label'", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /cssClass: \["day-label"\]/);
  assert.match(out, /text: model\.day/);
});

test("widgets: <Calendar> day delegate is T.AbstractButton with 32x32 implicit size", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /T\.AbstractButton \{/);
  assert.match(out, /implicitWidth: 32/);
  assert.match(out, /implicitHeight: 32/);
});

test("widgets: <Calendar> value={sig()} emits reactive __calVal0 property and :selected check", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} />;
    }
  `);
  assert.match(out, /property var __calVal0: d/);
  // :selected check compares model.year/month/day to the calVal Date
  assert.match(out, /__cal0\.__calVal0 instanceof Date/);
  assert.match(out, /model\.year === __cal0\.__calVal0\.getFullYear\(\)/);
  assert.match(out, /model\.month === __cal0\.__calVal0\.getMonth\(\)/);
  assert.match(out, /model\.day === __cal0\.__calVal0\.getDate\(\)/);
});

test("widgets: <Calendar> with no value emits __calVal0: null and falls back to today", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /property var __calVal0: null/);
  assert.match(out, /new Date\(\)\.getMonth\(\)/);
  assert.match(out, /new Date\(\)\.getFullYear\(\)/);
});

test("widgets: <Calendar> view month/year initialized from value when present", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} />;
    }
  `);
  assert.match(out, /property int __calMonth0: __calVal0 instanceof Date \? __calVal0\.getMonth\(\) : new Date\(\)\.getMonth\(\)/);
  assert.match(out, /property int __calYear0: __calVal0 instanceof Date \? __calVal0\.getFullYear\(\) : new Date\(\)\.getFullYear\(\)/);
});

test("widgets: <Calendar> onChange fires synthetic Date via new Date(model.year, model.month, model.day)", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} onChange={(e) => setD(e.target.value)} />;
    }
  `);
  assert.match(out, /onClicked: \{ d = new Date\(model\.year, model\.month, model\.day\) \}/);
});

test("widgets: <Calendar> onChange also handles e.target.valueAsDate", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} onChange={(e) => setD(e.target.valueAsDate)} />;
    }
  `);
  assert.match(out, /onClicked: \{ d = new Date\(model\.year, model\.month, model\.day\) \}/);
});

test("widgets: <Calendar class='my-cal'> passes class to wrapper cssClass", async () => {
  const out = await qml(`export function F(){ return <Calendar class="my-cal" />; }`);
  assert.match(out, /cssClass: \["my-cal"\]/);
});

test("widgets: <Calendar> Templates import uses version 6.3", async () => {
  const out = await qmlType(`export function F(){ return <Calendar />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.3 as T/);
});

test("widgets: <Calendar> is dispatched as builtin even when Calendar is in scope.components", async () => {
  // Calendar in BUILTIN_TAGS means isComponentIdentifier returns false for it, so scope.components
  // can contain it but the emitter dispatch fires our builtin BEFORE the user-component check.
  const { ast } = await normalize(`export function F() { return <Calendar />; }`, "f.tsx");
  const render = findRender(ast)!;
  const scope: Scope = {
    table: new Map(),
    mode: "binding",
    inputCounter: { n: 0 },
    hoverCounter: { n: 0 },
    components: new Map([["Calendar", "MyCalendarComp"]]),   // user component shadowed
    usedWidgets: { flag: false, calendar: false },
  };
  const out = emitQml(render, scope).join("\n");
  // Our builtin emits T.AbstractMonthGrid; user component MyCalendarComp must NOT appear.
  assert.match(out, /T\.AbstractMonthGrid/);
  assert.doesNotMatch(out, /MyCalendarComp/);
});

// ---------------------------------------------------------------------------
// Phase 5: <input type="date"> — readOnly field + calendar popup
// ---------------------------------------------------------------------------

test("widgets: <input type='date'> emits wrapper CssFill with cssPrimitive 'input'", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
});

test("widgets: <input type='date'> emits readOnly T.TextField inside the wrapper", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /T\.TextField \{/);
  assert.match(out, /readOnly: true/);
  assert.match(out, /background: null/);
});

test("widgets: <input type='date'> Binding formats value via Qt.formatDate", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /property: "text"/);
  assert.match(out, /value: __input0W\.__calVal0 instanceof Date \? Qt\.formatDate\(__input0W\.__calVal0, "yyyy-MM-dd"\) : ""/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: <input type='date'> emits chevron glyph anchored to the right", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /cssClass: \["chevron"\]/);
  assert.match(out, /text: "▾"/);
  assert.match(out, /anchors\.right: parent\.right/);
});

test("widgets: <input type='date'> MouseArea toggles popup open/close (with reopen guard)", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /onClicked: \{ __input0\.forceActiveFocus\(\); if \(__input0P\.visible\) __input0P\.close\(\); else if \(Date\.now\(\) - __input0P\.__closedAt > 150\) __input0P\.open\(\) \}/);
});

test("widgets: <input type='date'> cssState: focus when popup open, disabled when field disabled", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /__input0P\.visible \? \["focus"\]/);
  assert.match(out, /!__input0\.enabled \? \["disabled"\]/);
});

test("widgets: <input type='date'> popup is T.Popup below the field with padding 1 and .popup background", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /T\.Popup \{/);
  assert.match(out, /: __input0W\.height \+ 2/); // flip expression ends in the below-position
  assert.match(out, /padding: 1/);
  assert.match(out, /cssClass: \["popup"\]/);
});

test("widgets: <input type='date'> popup contains T.AbstractMonthGrid", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /T\.AbstractMonthGrid \{/);
});

test("widgets: <input type='date'> day click fires onChange and closes popup", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} onChange={(e) => setD(e.target.value)} />;
    }
  `);
  // onClick body must include both the onChange action and popup.close()
  assert.match(out, /d = new Date\(model\.year, model\.month, model\.day\)/);
  assert.match(out, /__input0P\.close\(\)/);
});

test("widgets: <input type='date'> popup day cssState carries today/selected/outside/hover", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /model\.today \? \["today"\]/);
  assert.match(out, /\["selected"\]/);
  assert.match(out, /model\.month !== __mg0\.month \? \["outside"\]/);
  assert.match(out, /__mgDel0\.hovered \? \["hover"\]/);
});

test("widgets: <input type='date'> disabled sets enabled: false on T.TextField", async () => {
  const out = await qml(`export function F(){ return <input type="date" disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: <input type='date'> Templates import uses version 6.3", async () => {
  const out = await qmlType(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.8 as T/);
});

test("widgets: <input type='date'> min throws a clear not-supported error", async () => {
  await assert.rejects(
    () => qml(`export function F(){ return <input type="date" min="2025-01-01" />; }`),
    /min\/max not supported yet/,
  );
});

test("widgets: <input type='date'> max throws a clear not-supported error", async () => {
  await assert.rejects(
    () => qml(`export function F(){ return <input type="date" max="2026-12-31" />; }`),
    /min\/max not supported yet/,
  );
});

// --- Phase 6.5 regression pins: slot internals must NOT be Css types (the CSS layout
// engine lays out every Css child, stomping anchors/geometry bindings); they are plain
// primitives styled via an injected CssItem. Popups must carry the implicit-size formula
// (Templates popups have none of their own). ---

test("emitQml 6.5: checkbox glyph is a plain Text styled via CssItem (not CssText)", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /Text \{[\s\S]*?text: "✓"[\s\S]*?Css\.CssItem \{ cssPrimitive: "text"; cssClass: \["indicator-glyph"\] \}/);
  assert.doesNotMatch(out, /Css\.CssText \{[\s\S]*?indicator-glyph/);
});

test("emitQml 6.5: switch knob is a plain Rectangle styled via CssItem, x follows visualPosition", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /Rectangle \{[\s\S]*?x: __input\d+\.visualPosition[\s\S]*?Css\.CssItem \{ cssPrimitive: "rect"; cssClass: \["knob"\] \}/);
  assert.doesNotMatch(out, /Css\.CssRect \{[\s\S]*?\["knob"\]/);
});

test("emitQml 6.5: radio dot is a plain Rectangle styled via CssItem with state-on-class", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  // The checked state rides the CssItem's own class (`.indicator-dot.checked`): the engine's
  // ancestor-restyle pass does not reach CssItems hosted under plain items.
  assert.match(out, /Rectangle \{[\s\S]*?Css\.CssItem \{ cssPrimitive: "rect"; cssClass: __input0\.checked \? \["indicator-dot", "checked"\] : \["indicator-dot"\] \}/);
});

test("emitQml 6.5: select popup carries the implicit-height formula", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /popup: T\.Popup \{[\s\S]*?implicitHeight: contentHeight \+ topPadding \+ bottomPadding/);
});

test("emitQml 6.5: spinbox pads for the buttons and steps on wheel only when focused", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /rightPadding: 32/);
  // Steps by writing `value` directly: Qt 6.11 dropped the Q_INVOKABLE from
  // increase()/decrease() (QQuickAbstractSpinBox refactor) — calling them is a TypeError.
  assert.match(out, /WheelHandler \{[\s\S]*?acceptedDevices: PointerDevice\.Mouse \| PointerDevice\.TouchPad[\s\S]*?valueModified\(\)/);
  assert.doesNotMatch(out, /increase\(\)/);
});

// --- Tab navigation (desktop): every interactive widget is a tab stop, bound to the
// solidTabstop context property (loader-owned) so `tabstop.enabled = false` opts the
// whole app out at runtime. ---

const TAB_STOP = /activeFocusOnTab: solidTabstop\.enabled/;

test("tabstop: <input> text field binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input />; }`), TAB_STOP);
});

test("tabstop: <textarea> binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <textarea />; }`), TAB_STOP);
});

test("tabstop: <input type='checkbox'> binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="checkbox" />; }`), TAB_STOP);
});

test("tabstop: switch binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`), TAB_STOP);
});

test("tabstop: radio binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="radio" name="g" />; }`), TAB_STOP);
});

test("tabstop: <select> binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <select><option>A</option></select>; }`), TAB_STOP);
});

test("tabstop: <input type='range'> binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="range" />; }`), TAB_STOP);
});

test("tabstop: <input type='number'> binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="number" />; }`), TAB_STOP);
});

test("tabstop: <input type='number'> forwards scope focus into the editable text", async () => {
  // T.SpinBox is a focus scope; without focus: true on the contentItem, tabbing into the
  // control leaves the TextInput unfocused and typing goes nowhere.
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /contentItem: TextInput \{[\s\S]*?focus: true/);
});

test("tabstop: <input type='date'> field is a tab stop and opens the popup from the keyboard", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, TAB_STOP);
  assert.match(out, /Keys\.onReturnPressed/);
  assert.match(out, /Keys\.onSpacePressed/);
});

test("tabstop: no widget hardcodes activeFocusOnTab: true", async () => {
  const out = await qml(`export function F(){ return <div><input /><textarea /><input type="number" /></div>; }`);
  assert.doesNotMatch(out, /activeFocusOnTab: true/);
});

// --- Calendar grids: Abstract templates instantiate NOTHING in C++ — the style must supply a
// contentItem whose Repeater binds control.source → control.delegate (verified in
// qtdeclarative/src/quicktemplates/qquickmonthgrid.cpp: the C++ only *resizes*
// contentItem->childItems(); positioning is the positioner's job). Without this the calendar
// renders header-only (the vertical collapse the owner reported). ---

test("calendar: MonthGrid gets a Grid(7x6)+Repeater contentItem wired to source/delegate", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /T\.AbstractMonthGrid \{[\s\S]*?contentItem: Grid \{[\s\S]*?columns: 7[\s\S]*?rows: 6[\s\S]*?Repeater \{[\s\S]*?model: __mg\d+\.source[\s\S]*?delegate: __mg\d+\.delegate/);
});

test("calendar: DayOfWeekRow gets a Row+Repeater contentItem wired to source/delegate", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /T\.AbstractDayOfWeekRow \{[\s\S]*?contentItem: Row \{[\s\S]*?Repeater \{[\s\S]*?model: __dow\d+\.source[\s\S]*?delegate: __dow\d+\.delegate/);
});

test("calendar: date input popup shares the same repeater-backed grids", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /contentItem: Grid \{[\s\S]*?Repeater \{[\s\S]*?model: __mg\d+\.source/);
});

test("calendar: cells size declaratively (incubated creation misses the C++ resizeItems)", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /delegate: Item \{[\s\S]*?width: \(__dow\d+\.contentItem\.width - 6 \* __dow\d+\.spacing\) \/ 7/);
  assert.match(out, /T\.AbstractButton \{[\s\S]*?width: \(__mg\d+\.contentItem\.width - 6 \* __mg\d+\.spacing\) \/ 7[\s\S]*?height: \(__mg\d+\.contentItem\.height - 5 \* __mg\d+\.spacing\) \/ 6/);
});

// --- Toggles must SHOW tab focus: AbstractButton already takes tab focus in C++
// (setActiveFocusOnTab(true) + StrongFocus in its constructor), but without "focus"
// in cssState there is no visual feedback and the tab stop looks dead. ---

test("tabstop: checkbox cssState carries focus alongside checked/disabled", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(__input0\.checked \? \["checked"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

test("tabstop: switch cssState carries focus", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(__input0\.checked/);
});

test("tabstop: radio cssState carries focus", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(__input0\.checked/);
});

// --- Ctrl+Tab: Qt Quick's tab chain explicitly SKIPS events with Control/Alt modifiers
// (qquickitem.cpp deliverKeyEvent), so Ctrl+Tab never navigates on its own. The Window
// emits a pair of Shortcuts walking nextItemInFocusChain — the desktop way out of a
// textarea, where plain Tab types. Respects the tabstop opt-out. ---

test("tabstop: Window emits Ctrl+Tab / Ctrl+Shift+Tab shortcuts honoring solidTabstop", async () => {
  const out = await qml(`export function F(){ return <Window title="t"><input /></Window>; }`);
  assert.match(out, /Shortcut \{[\s\S]*?sequences: \["Ctrl\+Tab"\][\s\S]*?enabled: solidTabstop\.enabled[\s\S]*?nextItemInFocusChain\(true\)/);
  assert.match(out, /Shortcut \{[\s\S]*?sequences: \["Ctrl\+Shift\+Tab", "Ctrl\+Backtab"\][\s\S]*?nextItemInFocusChain\(false\)/);
});

// --- Popup styling across the Overlay: Qt reparents popup contents to the window
// overlay, severing the visual chain that `.wg-select .popup` matches against. The
// emitted `property Item cssAncestor` re-anchors the engine's ancestor walk at the
// control; background and contentItem each carry it (they are SIBLING slots — every
// popup descendant walks through one of them). ---

test("select: popup background and contentItem re-anchor the CSS chain at the control", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /popup: T\.Popup \{[\s\S]*?background: Css\.CssFill \{[\s\S]*?property Item cssAncestor: __input0/);
  assert.match(out, /contentItem: ListView \{[\s\S]*?property Item cssAncestor: __input0/);
});

test("date: popup background and contentItem re-anchor the CSS chain at the wrapper", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /T\.Popup \{[\s\S]*?background: Css\.CssFill \{[\s\S]*?property Item cssAncestor: __input0W/);
  assert.match(out, /T\.Popup \{[\s\S]*?contentItem: Item \{[\s\S]*?property Item cssAncestor: __input0W/);
});

// --- Arrow keys (desktop): radios navigate AND check within their ButtonGroup (HTML/desktop
// semantics); checkbox/switch arrows move focus along the tab chain (dialog semantics),
// gated by the tabstop opt-out. Slider gains the same focused-wheel stepping as SpinBox. ---

test("arrows: radio steps its ButtonGroup (focus + check + toggled) on all four arrows", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /function __step\(d\) \{ var bs = __group_g\.buttons;[\s\S]*?forceActiveFocus\(Qt\.TabFocusReason\)[\s\S]*?checked = true;[\s\S]*?toggled\(\)/);
  assert.match(out, /Keys\.onDownPressed: __step\(1\)/);
  assert.match(out, /Keys\.onRightPressed: __step\(1\)/);
  assert.match(out, /Keys\.onUpPressed: __step\(-1\)/);
  assert.match(out, /Keys\.onLeftPressed: __step\(-1\)/);
});

test("arrows: radio without a name emits no arrow handlers", async () => {
  const out = await qml(`export function F(){ return <input type="radio" />; }`);
  assert.doesNotMatch(out, /Keys\.onDownPressed/);
});

test("arrows: checkbox and switch move focus along the chain, honoring the tabstop opt-out", async () => {
  for (const src of [`<input type="checkbox" />`, `<input type="checkbox" role="switch" />`]) {
    const out = await qml(`export function F(){ return ${src}; }`);
    assert.match(out, /Keys\.onDownPressed: \{ if \(solidTabstop\.enabled\) \{ var __n = __input0\.nextItemInFocusChain\(true\); if \(__n\) __n\.forceActiveFocus\(Qt\.TabFocusReason\) \} \}/);
    assert.match(out, /Keys\.onUpPressed: \{ if \(solidTabstop\.enabled\) \{ var __n = __input0\.nextItemInFocusChain\(false\)/);
  }
});

test("wheel: slider steps value when focused and re-emits moved()", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /WheelHandler \{[\s\S]*?acceptedDevices: PointerDevice\.Mouse \| PointerDevice\.TouchPad[\s\S]*?__input0\.moved\(\)/);
});

test("date: chevron is the same CssText as the select's, anchored right inside the host Item", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /Css\.CssText \{\n\s*cssPrimitive: ""\n\s*cssClass: \["chevron"\]\n\s*text: "▾"\n\s*anchors\.right: parent\.right\n\s*anchors\.rightMargin: 8/);
});

test("calendar: the day label carries the day states (sibling slots — no ancestor relation)", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /cssClass: \["day-label"\]\n\s*cssState: \(model\.today \? \["today"\] : \[\]\)/);
});

// --- Desktop popups: combo/date dropdowns are REAL windows (popupType: Popup.Window,
// Templates 6.8+) so they escape the app window, and they flip ABOVE the control when
// opening below would overflow the screen. ---

test("popups: select popup is a native window and flips above on screen overflow", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /popup: T\.Popup \{[\s\S]*?popupType: T\.Popup\.Window/);
  assert.match(out, /y: \(__input0\.mapToGlobal\(0, __input0\.height \+ 2\)\.y \+ height > Screen\.height\) \? -\(height \+ 2\) : __input0\.height \+ 2/);
});

test("popups: date popup is a native window and flips above on screen overflow", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /T\.Popup \{[\s\S]*?popupType: T\.Popup\.Window/);
  assert.match(out, /y: \(__input0W\.mapToGlobal\(0, __input0W\.height \+ 2\)\.y \+ height > Screen\.height\) \? -\(height \+ 2\) : __input0W\.height \+ 2/);
});

test("popups: widgets bump the Templates import to 6.8 (popupType)", async () => {
  const out = await qmlType(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /import QtQuick\.Templates 6\.8 as T/);
});

test("select: delegate binds highlighted to the combo's highlightedIndex (keyboard nav visible)", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /T\.ItemDelegate \{[\s\S]*?highlighted: __input0\.highlightedIndex === index/);
});

test("date: field click after a press-outside close does not reopen (toggle race)", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /property double __closedAt: 0/);
  assert.match(out, /onClosed: \{ __closedAt = Date\.now\(\)/);
  assert.match(out, /onClicked: \{ __input0\.forceActiveFocus\(\); if \(__input0P\.visible\) __input0P\.close\(\); else if \(Date\.now\(\) - __input0P\.__closedAt > 150\) __input0P\.open\(\) \}/);
});

// --- Date popup keyboard: arrows move a day cursor (±1 / ±7), Enter/Space commit it
// through the same onChange path a cell click uses; Down opens the closed popup. The
// popup keeps focus on the field, so the Keys live there. ---

test("date keyboard: field arrows step the cursor and Enter commits", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /function __calStep0\(days\)/);
  assert.match(out, /Keys\.onDownPressed: __input0P\.visible \? __input0W\.__calStep0\(7\) : __input0P\.open\(\)/);
  assert.match(out, /Keys\.onLeftPressed: \{ if \(__input0P\.visible\) __input0W\.__calStep0\(-1\) \}/);
  assert.match(out, /Keys\.onReturnPressed: __input0P\.visible \? __input0W\.__calCommit0\(\) : __input0P\.open\(\)/);
});

test("date keyboard: popup inits the cursor on open, clears on close, cell shows :focus", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /onOpened: __input0W\.__calCursor0 = __input0W\.__calVal0 instanceof Date \? __input0W\.__calVal0 : new Date\(\)/);
  assert.match(out, /onClosed: \{ __closedAt = Date\.now\(\); __input0W\.__calCursor0 = null \}/);
  assert.match(out, /\.concat\(\(__input0W\.__calCursor0 instanceof Date[\s\S]*?\) \? \["focus"\] : \[\]\)/);
});

test("date keyboard: Enter commit runs the author's onChange with the cursor date", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} onChange={(e) => setD(e.target.valueAsDate)} />;
    }
  `);
  assert.match(out, /function __calCommit0\(\) \{ if \(!\(__calCursor0 instanceof Date\)\) return; d = __calCursor0; __input0P\.close\(\) \}/);
});

test("date keyboard: field click gives the field focus (keyboard works after mouse open)", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /onClicked: \{ __input0\.forceActiveFocus\(\);/);
});

test("popups: dropdowns close when the app window deactivates (Qt::Popup semantics)", async () => {
  const sel = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(sel, /Window\.onActiveChanged: if \(!Window\.active\) __input0\.popup\.close\(\)/);
  const date = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(date, /Window\.onActiveChanged: if \(!Window\.active\) __input0P\.close\(\)/);
});
