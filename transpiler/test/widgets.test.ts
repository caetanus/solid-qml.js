/** Phase 2 widget tests: <input> and <textarea> → T.TextField / T.TextArea.
 *
 *  Pattern mirrors test/qml.test.ts: a local `qml()` helper for direct emitQml calls,
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
// <input> — wrapper shape and basic properties
// ---------------------------------------------------------------------------

const TEXTFIELD_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/TextField.qml", import.meta.url)), "utf8");
// Button is C++ now (widgets-to-cpp batch 2) — assertions read the C++ surface.
const BUTTON_CPP = await readFile(fileURLToPath(new URL("../../src/widgets/button.h", import.meta.url)), "utf8")
  + await readFile(fileURLToPath(new URL("../../src/widgets/button.cpp", import.meta.url)), "utf8");
const DIALOG_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Dialog.qml", import.meta.url)), "utf8");

test("default button: <button type=\"submit\"> emits isDefault; a plain button does not", async () => {
  const submit = await qml(`export function F(){ return <button type="submit">OK</button>; }`);
  assert.match(submit, /W\.Button \{[\s\S]*?isDefault: true[\s\S]*?text: "OK"/);
  const plain = await qml(`export function F(){ return <button>Cancel</button>; }`);
  assert.doesNotMatch(plain, /isDefault/);
});

test("default button: the C++ Button surfaces isDefault as a `default` cssState + a takeFocus()", async () => {
  assert.match(BUTTON_CPP, /Q_PROPERTY\(bool isDefault READ isDefault WRITE setIsDefault NOTIFY isDefaultChanged\)/);
  assert.match(BUTTON_CPP, /QStringLiteral\("default"\)/);
  assert.match(BUTTON_CPP, /Q_INVOKABLE void takeFocus\(\)/);
});

test("default button: Dialog fires the default on Enter (bubbled) and focuses it on open", async () => {
  assert.match(DIALOG_QML, /function __findDefault\(item\)/);
  assert.match(DIALOG_QML, /Keys\.onReturnPressed:.*__findDefault\(root\).*b\.clicked\(\)/);
  assert.match(DIALOG_QML, /Keys\.onEnterPressed:.*__findDefault\(root\).*b\.clicked\(\)/);
  assert.match(DIALOG_QML, /var def = wrap\.__findDefault\(root\);[\s\S]*?def\.takeFocus\(\)/);
});

test("widgets: <input> instantiates the W.TextField component", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  assert.match(out, /W\.TextField \{/);
  assert.match(out, /id: __input0/);
  // The T.TextField internals now live in TextField.qml, not the emit.
  assert.doesNotMatch(out, /T\.TextField/);
});

test("widgets: TextField.qml wrapper carries cssPrimitive 'input' and focus/disabled cssState", async () => {
  assert.match(TEXTFIELD_QML, /cssPrimitive: "input"/);
  assert.match(TEXTFIELD_QML, /cssState: \(field\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!field\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: TextField.qml mirrors implicitWidth/Height from the control", async () => {
  assert.match(TEXTFIELD_QML, /implicitWidth: field\.implicitWidth/);
  assert.match(TEXTFIELD_QML, /implicitHeight: field\.implicitHeight/);
});

test("widgets: TextField.qml has a T.TextField with background null", async () => {
  assert.match(TEXTFIELD_QML, /T\.TextField \{/);
  assert.match(TEXTFIELD_QML, /anchors\.fill: parent/);
  assert.match(TEXTFIELD_QML, /background: null/);
});

test("widgets: TextField.qml bridges CSS colour and font from the wrapper's inherited properties", async () => {
  assert.match(TEXTFIELD_QML, /color: cssTheme\.parseColor\(root\.inheritedColor \|\| "#2b2b2b"\)/);
  assert.match(TEXTFIELD_QML, /font\.family: cssTheme\.resolveFontFamily\(root\.inheritedFontFamily/);
  assert.match(TEXTFIELD_QML, /font\.pixelSize: cssTheme\.parseFontSize\(root\.inheritedFontSize/);
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

test("widgets: TextField.qml cssState carries the disabled concat (runtime-evaluated from enabled)", async () => {
  assert.match(TEXTFIELD_QML, /!field\.enabled \? \["disabled"\] : \[\]/);
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

test("widgets: placeholder sets the component's placeholder prop; overlay lives in TextField.qml", async () => {
  const out = await qml(`export function F(){ return <input placeholder="Type here" />; }`);
  assert.match(out, /placeholder: "Type here"/);
  // The overlay Text (hides on focus / when text present) lives in TextField.qml.
  assert.match(TEXTFIELD_QML, /visible: parent\.text\.length === 0 && !parent\.activeFocus/);
});

test("widgets: <input> without placeholder does NOT set the placeholder prop", async () => {
  const out = await qml(`export function F(){ return <input />; }`);
  // No placeholder attribute → no placeholder line emitted.
  assert.doesNotMatch(out, /placeholder:/);
  const textCount = (out.match(/Text \{/g) ?? []).length;
  assert.equal(textCount, 0);
});

// ---------------------------------------------------------------------------
// <textarea>
// ---------------------------------------------------------------------------

const TEXTAREA_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/TextArea.qml", import.meta.url)), "utf8");

test("widgets: <textarea> instantiates the W.TextArea component", async () => {
  const out = await qml(`export function F(){ return <textarea></textarea>; }`);
  assert.match(out, /W\.TextArea \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.TextArea/);
});

test("widgets: TextArea.qml has a T.TextArea with wrapMode, background null, cssPrimitive 'textarea'", async () => {
  assert.match(TEXTAREA_QML, /cssPrimitive: "textarea"/);
  assert.match(TEXTAREA_QML, /T\.TextArea \{/);
  assert.match(TEXTAREA_QML, /wrapMode: TextEdit\.Wrap/);
  assert.match(TEXTAREA_QML, /background: null/);
});

test("widgets: TextArea.qml cssState includes focus and disabled (same pattern as input)", async () => {
  assert.match(TEXTAREA_QML, /cssState: \(field\.activeFocus \? \["focus"\] : \[\]\)/);
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

test("widgets: <textarea placeholder='...'> sets the placeholder prop; overlay lives in TextArea.qml", async () => {
  const out = await qml(`export function F(){ return <textarea placeholder="Notes..."></textarea>; }`);
  assert.match(out, /placeholder: "Notes\.\.\."/);
  assert.match(TEXTAREA_QML, /anchors\.top: parent\.top/);
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

test("widgets: Widgets import is prepended when component contains an <input>", async () => {
  const out = await qmlType(`
    export function F() {
      return <input />;
    }
  `);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  // The T.TextField control lives in TextField.qml now — no Templates import in the component.
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});

test("widgets: Widgets import is prepended when component contains a <textarea>", async () => {
  const out = await qmlType(`
    export function F() {
      return <textarea></textarea>;
    }
  `);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
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

const CHECKBOX_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Checkbox.qml", import.meta.url)), "utf8");
const TOGGLE_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Toggle.qml", import.meta.url)), "utf8");

test("widgets: <input type='checkbox'> instantiates the W.Checkbox component", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /W\.Checkbox \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.CheckBox/);
});

test("widgets: Checkbox.qml has T.CheckBox with background/contentItem null and cssPrimitive 'input'", async () => {
  assert.match(CHECKBOX_QML, /cssPrimitive: "input"/);
  assert.match(CHECKBOX_QML, /T\.CheckBox \{/);
  assert.match(CHECKBOX_QML, /background: null/);
  assert.match(CHECKBOX_QML, /contentItem: null/);
});

test("widgets: Checkbox.qml cssState carries 'checked' and 'disabled' pseudo-classes", async () => {
  assert.match(CHECKBOX_QML, /box\.checked \? \["checked"\] : \[\]/);
  assert.match(CHECKBOX_QML, /!box\.enabled \? \["disabled"\] : \[\]/);
});

test("widgets: Checkbox.qml indicator CssFill has explicit width/height 20 + implicitWidth/Height 20", async () => {
  assert.match(CHECKBOX_QML, /cssClass: \["indicator"\]/);
  assert.match(CHECKBOX_QML, /width: 20/);
  assert.match(CHECKBOX_QML, /height: 20/);
  assert.match(CHECKBOX_QML, /implicitWidth: 20/);
  assert.match(CHECKBOX_QML, /implicitHeight: 20/);
});

test("widgets: Checkbox.qml indicator contains a plain Text glyph (CssItem) visible when checked", async () => {
  assert.match(CHECKBOX_QML, /cssClass: \["indicator-glyph"\]/);
  assert.match(CHECKBOX_QML, /text: "✓"/);
  assert.match(CHECKBOX_QML, /visible: box\.checked/);
  assert.match(CHECKBOX_QML, /anchors\.centerIn: parent/);
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

test("widgets: checkbox Widgets import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="checkbox" />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});

// ---------------------------------------------------------------------------
// Phase 3: <input type="checkbox" role="switch"> — switch toggle
// ---------------------------------------------------------------------------

test("widgets: <input role='switch'> instantiates the W.Toggle component", async () => {
  const out = await qml(`export function F(){ return <input type="checkbox" role="switch" />; }`);
  assert.match(out, /W\.Toggle \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.Switch/);
});

test("widgets: Toggle.qml has T.Switch + track CssFill (cssClass 'track') + knob CssItem", async () => {
  assert.match(TOGGLE_QML, /T\.Switch \{/);
  assert.match(TOGGLE_QML, /cssClass: \["track"\]/);
  assert.match(TOGGLE_QML, /cssClass: \["knob"\]/);
});

test("widgets: Toggle.qml track is 36x20 and knob is 16x16 (hardcoded — not in Css container)", async () => {
  assert.match(TOGGLE_QML, /width: 36/);
  assert.match(TOGGLE_QML, /height: 20/);
  assert.match(TOGGLE_QML, /implicitWidth: 36/);
  assert.match(TOGGLE_QML, /implicitHeight: 20/);
  assert.match(TOGGLE_QML, /width: 16/);
  assert.match(TOGGLE_QML, /height: 16/);
});

test("widgets: Toggle.qml knob x uses visualPosition binding for animated slide", async () => {
  assert.match(TOGGLE_QML, /x: sw\.visualPosition \* \(parent\.width - width\)/);
});

test("widgets: Toggle.qml knob has Behavior on x with NumberAnimation 120ms", async () => {
  assert.match(TOGGLE_QML, /Behavior on x \{ NumberAnimation \{ duration: 120 \} \}/);
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

test("widgets: Toggle.qml cssState carries focus/checked/disabled", async () => {
  assert.match(TOGGLE_QML, /\(sw\.activeFocus \? \["focus"\] : \[\]\)\.concat\(sw\.checked \? \["checked"\] : \[\]\)\.concat\(!sw\.enabled \? \["disabled"\] : \[\]\)/);
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
  const groupDeclCount = (out.match(/T\.ButtonGroup \{ id: __group_a;/g) ?? []).length;
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
  assert.match(out, /T\.ButtonGroup \{ id: __group_a;/);
  assert.match(out, /T\.ButtonGroup \{ id: __group_b;/);
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

test("widgets: radio onChange fires via onCheckedChanged guarded on checked (arrow-nav propagates)", async () => {
  const out = await qmlType(`
    export function F() {
      const [sel, setSel] = createSignal(false);
      return <input type="radio" name="x" checked={sel()} onChange={(e) => setSel(e.target.checked)} />;
    }
  `);
  // onToggled would miss arrow-nav (programmatic checked doesn't emit toggled); guard on checked so
  // the auto-unchecked sibling doesn't fire and a controlled re-assert is a no-op.
  assert.match(out, /onCheckedChanged: \{ if \(__input0\.checked\) \{ sel = __input0\.checked \} \}/);
});

test("widgets: radio Templates import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="radio" name="x" />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

// ---------------------------------------------------------------------------
// Phase 4: <select> / <option> → T.ComboBox
// ---------------------------------------------------------------------------

const SELECT_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Select.qml", import.meta.url)), "utf8");

test("widgets: <select> instantiates the W.Select component", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /W\.Select \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.ComboBox/);
});

test("widgets: Select.qml has a T.ComboBox with background null and leftPadding 12", async () => {
  assert.match(SELECT_QML, /cssPrimitive: "select"/);
  assert.match(SELECT_QML, /T\.ComboBox \{/);
  assert.match(SELECT_QML, /anchors\.fill: parent/);
  assert.match(SELECT_QML, /background: null/);
  assert.match(SELECT_QML, /leftPadding: 12/);
});

test("widgets: <select> model array contains option labels in order", async () => {
  const out = await qml(`export function F(){ return (
    <select><option value="a">Alpha</option><option value="b">Beta</option></select>
  ); }`);
  assert.match(out, /model: \["Alpha", "Beta"\]/);
});

test("widgets: <select> values array holds option values", async () => {
  const out = await qml(`export function F(){ return (
    <select><option value="a">Alpha</option><option value="b">Beta</option></select>
  ); }`);
  assert.match(out, /values: \["a", "b"\]/);
});

test("widgets: <option> without value attr uses the label as value", async () => {
  const out = await qml(`export function F(){ return (
    <select><option>Baz</option></select>
  ); }`);
  // Both model label and values entry should be "Baz"
  assert.match(out, /model: \["Baz"\]/);
  assert.match(out, /values: \["Baz"\]/);
});

test("widgets: Select.qml contentItem is CssText with cssClass ['value'] showing displayText", async () => {
  assert.match(SELECT_QML, /contentItem: Css\.CssText \{/);
  assert.match(SELECT_QML, /cssClass: \["value"\]/);
  assert.match(SELECT_QML, /text: ctl\.displayText/);
});

test("widgets: Select.qml emits a chevron CssText with cssClass ['chevron'] anchored right", async () => {
  assert.match(SELECT_QML, /cssClass: \["chevron"\]/);
  assert.match(SELECT_QML, /text: "▾"/);
  assert.match(SELECT_QML, /anchors\.right: parent\.right/);
  assert.match(SELECT_QML, /anchors\.verticalCenter: parent\.verticalCenter/);
});

test("widgets: Select.qml delegate is T.ItemDelegate with explicit width from popup.width", async () => {
  assert.match(SELECT_QML, /delegate: T\.ItemDelegate \{/);
  assert.match(SELECT_QML, /width: ctl\.popup\.width/);
  assert.match(SELECT_QML, /implicitHeight: 36/);
});

test("widgets: Select.qml delegate background CssFill has cssClass ['option'] and hover/selected cssState", async () => {
  assert.match(SELECT_QML, /cssClass: \["option"\]/);
  assert.match(SELECT_QML, /optDel\.highlighted \? \["hover"\] : \[\]/);
  assert.match(SELECT_QML, /ctl\.currentIndex === index \? \["selected"\] : \[\]/);
});

test("widgets: Select.qml delegate contentItem is CssText with cssClass ['option-label']", async () => {
  assert.match(SELECT_QML, /cssClass: \["option-label"\]/);
  assert.match(SELECT_QML, /text: modelData/);
});

test("widgets: Select.qml popup is T.Popup with y below (flip expr), width = combo.width, padding 1", async () => {
  assert.match(SELECT_QML, /popup: T\.Popup \{/);
  assert.match(SELECT_QML, /: ctl\.height \+ 2/); // flip expression ends in the below-position
  assert.match(SELECT_QML, /width: ctl\.width/);
  assert.match(SELECT_QML, /padding: 1/);
});

test("widgets: Select.qml popup background is CssFill cssClass ['popup']", async () => {
  assert.match(SELECT_QML, /cssClass: \["popup"\]/);
});

test("widgets: Select.qml popup contentItem is ListView with delegateModel and capped implicitHeight", async () => {
  assert.match(SELECT_QML, /contentItem: ListView \{/);
  assert.match(SELECT_QML, /model: ctl\.delegateModel/);
  assert.match(SELECT_QML, /currentIndex: ctl\.highlightedIndex/);
  assert.match(SELECT_QML, /implicitHeight: Math\.min\(contentHeight, 240\)/);
});

test("widgets: Select.qml cssState carries focus and disabled on the wrapper", async () => {
  assert.match(SELECT_QML, /ctl\.activeFocus \? \["focus"\] : \[\]/);
  assert.match(SELECT_QML, /!ctl\.enabled \? \["disabled"\] : \[\]/);
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
  assert.match(out, /value: __input0\.values\.indexOf\(sel\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: <select> without value does NOT emit a Binding", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.doesNotMatch(out, /Binding \{/);
});

test("widgets: <select> onChange wires onActivated with e.target.value → values[index]", async () => {
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
  assert.match(out, /onActivated: \(index\) => \{ sel = __input0\.values\[index\] \}/);
});

test("widgets: <select> without onChange does NOT emit onActivated", async () => {
  const out = await qml(`export function F(){ return <select><option>A</option></select>; }`);
  assert.doesNotMatch(out, /onActivated/);
});

test("widgets: <select> disabled prop sets enabled: false on the instance", async () => {
  const out = await qml(`export function F(){ return <select disabled><option>A</option></select>; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: <select> Widgets import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <select><option>A</option></select>; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
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

const SLIDER_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Slider.qml", import.meta.url)), "utf8");

test("widgets: <input type='range'> instantiates the W.Slider component", async () => {
  const out = await qml(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /W\.Slider \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.Slider/);
});

test("widgets: Slider.qml has a T.Slider (anchors.fill) with cssPrimitive 'input'", async () => {
  assert.match(SLIDER_QML, /cssPrimitive: "input"/);
  assert.match(SLIDER_QML, /T\.Slider \{/);
  assert.match(SLIDER_QML, /anchors\.fill: parent/);
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

test("widgets: Slider.qml background is CssFill cssClass ['track'] with height 6", async () => {
  assert.match(SLIDER_QML, /background: Css\.CssFill \{/);
  assert.match(SLIDER_QML, /cssClass: \["track"\]/);
  assert.match(SLIDER_QML, /height: 6/);
  assert.match(SLIDER_QML, /implicitHeight: 6/);
});

test("widgets: Slider.qml track background uses Qt-Basic-style x/y/width geometry", async () => {
  assert.match(SLIDER_QML, /x: ctl\.leftPadding/);
  assert.match(SLIDER_QML, /y: ctl\.topPadding \+ \(ctl\.availableHeight - height\) \/ 2/);
  assert.match(SLIDER_QML, /width: ctl\.availableWidth/);
});

test("widgets: Slider.qml track contains CssRect cssClass ['track-fill'] width driven by visualPosition", async () => {
  assert.match(SLIDER_QML, /cssClass: \["track-fill"\]/);
  assert.match(SLIDER_QML, /width: ctl\.visualPosition \* parent\.width/);
});

test("widgets: Slider.qml handle is CssRect cssClass ['handle'] 18x18", async () => {
  assert.match(SLIDER_QML, /handle: Css\.CssRect \{/);
  assert.match(SLIDER_QML, /cssClass: \["handle"\]/);
  assert.match(SLIDER_QML, /width: 18/);
  assert.match(SLIDER_QML, /height: 18/);
  assert.match(SLIDER_QML, /implicitWidth: 18/);
  assert.match(SLIDER_QML, /implicitHeight: 18/);
});

test("widgets: Slider.qml handle x/y use visualPosition and availableWidth/Height", async () => {
  assert.match(SLIDER_QML, /x: ctl\.leftPadding \+ ctl\.visualPosition \* \(ctl\.availableWidth - width\)/);
  assert.match(SLIDER_QML, /y: ctl\.topPadding \+ ctl\.availableHeight \/ 2 - height \/ 2/);
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

test("widgets: Slider.qml cssState carries focus and disabled", async () => {
  assert.match(SLIDER_QML, /cssState: \(ctl\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!ctl\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: range Widgets import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="range" />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});

// ---------------------------------------------------------------------------
// Phase 4: <input type="number"> → T.SpinBox
// ---------------------------------------------------------------------------

const SPINBOX_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/SpinBox.qml", import.meta.url)), "utf8");

test("widgets: <input type='number'> instantiates the W.SpinBox component", async () => {
  const out = await qml(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /W\.SpinBox \{/);
  assert.match(out, /id: __input0/);
  assert.doesNotMatch(out, /T\.SpinBox/);
});

test("widgets: SpinBox.qml has a T.SpinBox with editable:true and background:null", async () => {
  assert.match(SPINBOX_QML, /cssPrimitive: "input"/);
  assert.match(SPINBOX_QML, /T\.SpinBox \{/);
  assert.match(SPINBOX_QML, /anchors\.fill: parent/);
  assert.match(SPINBOX_QML, /background: null/);
  assert.match(SPINBOX_QML, /editable: true/);
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

test("widgets: SpinBox.qml contentItem is TextInput with displayText and validator", async () => {
  assert.match(SPINBOX_QML, /contentItem: TextInput \{/);
  assert.match(SPINBOX_QML, /text: ctl\.displayText/);
  assert.match(SPINBOX_QML, /validator: ctl\.validator/);
  assert.match(SPINBOX_QML, /readOnly: !ctl\.editable/);
});

test("widgets: SpinBox.qml contentItem bridges color/font from the CssFill wrapper (root)", async () => {
  assert.match(SPINBOX_QML, /color: cssTheme\.parseColor\(root\.inheritedColor \|\| "#2b2b2b"\)/);
  assert.match(SPINBOX_QML, /font\.family: cssTheme\.resolveFontFamily\(root\.inheritedFontFamily/);
  assert.match(SPINBOX_QML, /font\.pixelSize: cssTheme\.parseFontSize\(root\.inheritedFontSize/);
});

test("widgets: SpinBox.qml up.indicator is CssFill cssClass ['spin-up'] at top-right", async () => {
  assert.match(SPINBOX_QML, /up\.indicator: Css\.CssFill \{/);
  assert.match(SPINBOX_QML, /cssClass: \["spin-up"\]/);
  assert.match(SPINBOX_QML, /x: parent\.width - width - 2/);
  assert.match(SPINBOX_QML, /y: 2/);
  assert.match(SPINBOX_QML, /width: 24/);
});

test("widgets: SpinBox.qml down.indicator is CssFill cssClass ['spin-down'] at bottom-right", async () => {
  assert.match(SPINBOX_QML, /down\.indicator: Css\.CssFill \{/);
  assert.match(SPINBOX_QML, /cssClass: \["spin-down"\]/);
  assert.match(SPINBOX_QML, /y: parent\.height \/ 2/);
});

test("widgets: SpinBox.qml indicators contain plain '+' and '−' glyphs (CssItem) centred", async () => {
  assert.match(SPINBOX_QML, /cssClass: \["spin-glyph"\]/);
  assert.match(SPINBOX_QML, /text: "\+"/);
  assert.match(SPINBOX_QML, /text: "−"/);
  assert.match(SPINBOX_QML, /anchors\.centerIn: parent/);
});

test("widgets: SpinBox.qml up/down indicators cssState carries 'active' when pressed", async () => {
  assert.match(SPINBOX_QML, /ctl\.up\.pressed \? \["active"\] : \[\]/);
  assert.match(SPINBOX_QML, /ctl\.down\.pressed \? \["active"\] : \[\]/);
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

test("widgets: SpinBox.qml cssState carries focus and disabled", async () => {
  assert.match(SPINBOX_QML, /cssState: \(ctl\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!ctl\.enabled \? \["disabled"\] : \[\]\)/);
});

test("widgets: number Widgets import is prepended", async () => {
  const out = await qmlType(`export function F(){ return <input type="number" />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});

// ---------------------------------------------------------------------------
// Phase 5: <Calendar> — inline month grid
// ---------------------------------------------------------------------------

const MONTHGRID_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/MonthGrid.qml", import.meta.url)), "utf8");
const CALENDAR_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Calendar.qml", import.meta.url)), "utf8");
const DATEFIELD_QML = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/DateField.qml", import.meta.url)), "utf8");

test("widgets: <Calendar> instantiates the W.Calendar component (wrapper cssPrimitive 'div')", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.match(out, /W\.Calendar \{/);
  assert.doesNotMatch(out, /T\.AbstractMonthGrid/);
  assert.match(CALENDAR_QML, /cssPrimitive: "div"/);
});

test("widgets: MonthGrid.qml emits T.AbstractMonthGrid with month and year bound to view properties", async () => {
  assert.match(MONTHGRID_QML, /T\.AbstractMonthGrid \{/);
  assert.match(MONTHGRID_QML, /month: root\.viewMonth/);
  assert.match(MONTHGRID_QML, /year: root\.viewYear/);
});

test("widgets: MonthGrid.qml T.AbstractMonthGrid carries height: parent.height - 56 so rows are visible", async () => {
  assert.match(MONTHGRID_QML, /height: parent\.height - 56/);
});

test("widgets: MonthGrid.qml emits T.AbstractDayOfWeekRow with Css.CssText delegate", async () => {
  assert.match(MONTHGRID_QML, /T\.AbstractDayOfWeekRow \{/);
  assert.match(MONTHGRID_QML, /cssClass: \["dow"\]/);
  assert.match(MONTHGRID_QML, /text: model\.shortName/);
});

test("widgets: MonthGrid.qml emits prev/next cal-nav buttons and cal-title label", async () => {
  assert.match(MONTHGRID_QML, /cssClass: \["cal-nav"\]/);
  assert.match(MONTHGRID_QML, /cssClass: \["cal-title"\]/);
  assert.match(MONTHGRID_QML, /text: "‹"/);
  assert.match(MONTHGRID_QML, /text: "›"/);
});

test("widgets: MonthGrid.qml nav prev button wraps Dec→Jan on month 0", async () => {
  assert.match(MONTHGRID_QML, /root\.viewMonth === 0/);
  assert.match(MONTHGRID_QML, /root\.viewYear = root\.viewYear - 1/);
  assert.match(MONTHGRID_QML, /root\.viewMonth = 11/);
});

test("widgets: MonthGrid.qml nav next button wraps Dec→Jan on month 11", async () => {
  assert.match(MONTHGRID_QML, /root\.viewMonth === 11/);
  assert.match(MONTHGRID_QML, /root\.viewYear = root\.viewYear \+ 1/);
  assert.match(MONTHGRID_QML, /root\.viewMonth = 0/);
});

test("widgets: MonthGrid.qml day delegate carries cssState: today/selected/outside/hover", async () => {
  assert.match(MONTHGRID_QML, /cssClass: \["day"\]/);
  assert.match(MONTHGRID_QML, /model\.today \? \["today"\]/);
  assert.match(MONTHGRID_QML, /\["selected"\]/);
  assert.match(MONTHGRID_QML, /model\.month !== mg\.month \? \["outside"\]/);
  assert.match(MONTHGRID_QML, /mgDel\.hovered \? \["hover"\]/);
});

test("widgets: MonthGrid.qml day delegate contentItem is Css.CssText with class 'day-label'", async () => {
  assert.match(MONTHGRID_QML, /cssClass: \["day-label"\]/);
  assert.match(MONTHGRID_QML, /text: model\.day/);
});

test("widgets: MonthGrid.qml day delegate is T.AbstractButton with 32x32 implicit size", async () => {
  assert.match(MONTHGRID_QML, /T\.AbstractButton \{/);
  assert.match(MONTHGRID_QML, /implicitWidth: 32/);
  assert.match(MONTHGRID_QML, /implicitHeight: 32/);
});

test("widgets: <Calendar> value={sig()} sets the value prop; MonthGrid.qml carries the :selected check", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} />;
    }
  `);
  assert.match(out, /value: d/);
  // :selected check compares model.year/month/day to the selectedDate Date (in MonthGrid.qml).
  assert.match(MONTHGRID_QML, /root\.selectedDate instanceof Date/);
  assert.match(MONTHGRID_QML, /model\.year === root\.selectedDate\.getFullYear\(\)/);
  assert.match(MONTHGRID_QML, /model\.month === root\.selectedDate\.getMonth\(\)/);
  assert.match(MONTHGRID_QML, /model\.day === root\.selectedDate\.getDate\(\)/);
});

test("widgets: <Calendar> with no value does NOT set value; MonthGrid.qml falls back to today", async () => {
  const out = await qml(`export function F(){ return <Calendar />; }`);
  assert.doesNotMatch(out, /value:/);
  assert.match(MONTHGRID_QML, /new Date\(\)\.getMonth\(\)/);
  assert.match(MONTHGRID_QML, /new Date\(\)\.getFullYear\(\)/);
});

test("widgets: MonthGrid.qml view month/year initialized from selectedDate when present", async () => {
  assert.match(MONTHGRID_QML, /property int viewMonth: selectedDate instanceof Date \? selectedDate\.getMonth\(\) : new Date\(\)\.getMonth\(\)/);
  assert.match(MONTHGRID_QML, /property int viewYear: selectedDate instanceof Date \? selectedDate\.getFullYear\(\) : new Date\(\)\.getFullYear\(\)/);
});

test("widgets: <Calendar> onChange fires onDayPicked with the picked Date (e.target.value → date)", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} onChange={(e) => setD(e.target.value)} />;
    }
  `);
  assert.match(out, /onDayPicked: \(date\) => \{ d = date \}/);
  // MonthGrid.qml computes the Date and fires dayPicked on click.
  assert.match(MONTHGRID_QML, /onClicked: root\.dayPicked\(new Date\(model\.year, model\.month, model\.day\)\)/);
});

test("widgets: <Calendar> onChange also handles e.target.valueAsDate", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <Calendar value={d()} onChange={(e) => setD(e.target.valueAsDate)} />;
    }
  `);
  assert.match(out, /onDayPicked: \(date\) => \{ d = date \}/);
});

test("widgets: <Calendar class='my-cal'> passes class to wrapper cssClass", async () => {
  const out = await qml(`export function F(){ return <Calendar class="my-cal" />; }`);
  assert.match(out, /cssClass: \["my-cal"\]/);
});

test("widgets: <Calendar> Widgets import is prepended (Calendar.qml owns the Templates import)", async () => {
  const out = await qmlType(`export function F(){ return <Calendar />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
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
    usedWidgets: { flag: false, calendar: false, widgetLib: false },
  };
  const out = emitQml(render, scope).join("\n");
  // Our builtin instantiates W.Calendar; user component MyCalendarComp must NOT appear.
  assert.match(out, /W\.Calendar/);
  assert.doesNotMatch(out, /MyCalendarComp/);
});

// ---------------------------------------------------------------------------
// Phase 5: <input type="date"> — readOnly field + calendar popup
// ---------------------------------------------------------------------------

test("widgets: <input type='date'> instantiates the W.DateField component", async () => {
  const out = await qml(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /W\.DateField \{/);
  assert.doesNotMatch(out, /T\.TextField/);
  assert.match(DATEFIELD_QML, /cssPrimitive: "input"/);
});

test("widgets: DateField.qml has a readOnly T.TextField inside the wrapper", async () => {
  assert.match(DATEFIELD_QML, /T\.TextField \{/);
  assert.match(DATEFIELD_QML, /readOnly: true/);
  assert.match(DATEFIELD_QML, /background: null/);
});

test("widgets: <input type='date'> value={sig()} sets the value prop; DateField.qml formats via Qt.formatDate", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} />;
    }
  `);
  assert.match(out, /value: d/);
  assert.match(DATEFIELD_QML, /Binding \{/);
  assert.match(DATEFIELD_QML, /property: "text"/);
  assert.match(DATEFIELD_QML, /value: root\.value instanceof Date \? Qt\.formatDate\(root\.value, "yyyy-MM-dd"\) : ""/);
  assert.match(DATEFIELD_QML, /restoreMode: Binding\.RestoreNone/);
});

test("widgets: DateField.qml emits chevron glyph anchored to the right", async () => {
  assert.match(DATEFIELD_QML, /cssClass: \["chevron"\]/);
  assert.match(DATEFIELD_QML, /text: "▾"/);
  assert.match(DATEFIELD_QML, /anchors\.right: parent\.right/);
});

test("widgets: DateField.qml MouseArea toggles popup open/close (with reopen guard)", async () => {
  assert.match(DATEFIELD_QML, /onClicked: \{ field\.forceActiveFocus\(\); if \(pop\.visible\) pop\.close\(\); else if \(Date\.now\(\) - pop\.__closedAt > 150\) pop\.open\(\) \}/);
});

test("widgets: DateField.qml cssState: focus when popup open, disabled when field disabled", async () => {
  assert.match(DATEFIELD_QML, /pop\.visible \? \["focus"\]/);
  assert.match(DATEFIELD_QML, /!field\.enabled \? \["disabled"\]/);
});

test("widgets: DateField.qml popup is T.Popup below the field with padding 1 and .popup background", async () => {
  assert.match(DATEFIELD_QML, /T\.Popup \{/);
  assert.match(DATEFIELD_QML, /: root\.height \+ 2/); // flip expression ends in the below-position
  assert.match(DATEFIELD_QML, /padding: 1/);
  assert.match(DATEFIELD_QML, /cssClass: \["popup"\]/);
});

test("widgets: DateField.qml popup contentItem is the shared MonthGrid", async () => {
  assert.match(DATEFIELD_QML, /contentItem: MonthGrid \{/);
});

test("widgets: <input type='date'> day click fires onDayPicked; DateField.qml closes the popup", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} onChange={(e) => setD(e.target.value)} />;
    }
  `);
  assert.match(out, /onDayPicked: \(date\) => \{ d = date \}/);
  // MonthGrid's dayPicked routes through DateField.qml, which relays to the emit handler then closes.
  assert.match(DATEFIELD_QML, /onDayPicked: \(date\) => \{ root\.dayPicked\(date\); pop\.close\(\) \}/);
});

test("widgets: MonthGrid.qml day cssState carries today/selected/outside/hover (used by the date popup)", async () => {
  assert.match(MONTHGRID_QML, /model\.today \? \["today"\]/);
  assert.match(MONTHGRID_QML, /\["selected"\]/);
  assert.match(MONTHGRID_QML, /model\.month !== mg\.month \? \["outside"\]/);
  assert.match(MONTHGRID_QML, /mgDel\.hovered \? \["hover"\]/);
});

test("widgets: <input type='date'> disabled sets enabled: false on the instance", async () => {
  const out = await qml(`export function F(){ return <input type="date" disabled />; }`);
  assert.match(out, /enabled: false/);
});

test("widgets: <input type='date'> Widgets import is prepended (DateField.qml owns the Templates import)", async () => {
  const out = await qmlType(`export function F(){ return <input type="date" />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
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

test("emitQml 6.5: Checkbox.qml glyph is a plain Text styled via CssItem (not CssText)", async () => {
  assert.match(CHECKBOX_QML, /Text \{[\s\S]*?text: "✓"[\s\S]*?Css\.CssItem \{ cssPrimitive: "text"; cssClass: \["indicator-glyph"\] \}/);
  assert.doesNotMatch(CHECKBOX_QML, /Css\.CssText \{[\s\S]*?indicator-glyph/);
});

test("emitQml 6.5: Toggle.qml knob is a plain Rectangle styled via CssItem, x follows visualPosition", async () => {
  assert.match(TOGGLE_QML, /Rectangle \{[\s\S]*?x: sw\.visualPosition[\s\S]*?Css\.CssItem \{ cssPrimitive: "rect"; cssClass: \["knob"\] \}/);
  assert.doesNotMatch(TOGGLE_QML, /Css\.CssRect \{[\s\S]*?\["knob"\]/);
});

test("emitQml 6.5: radio dot is a plain Rectangle styled via CssItem with state-on-class", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  // The checked state rides the CssItem's own class (`.indicator-dot.checked`): the engine's
  // ancestor-restyle pass does not reach CssItems hosted under plain items.
  assert.match(out, /Rectangle \{[\s\S]*?Css\.CssItem \{ cssPrimitive: "rect"; cssClass: __input0\.checked \? \["indicator-dot", "checked"\] : \["indicator-dot"\] \}/);
});

test("emitQml 6.5: Select.qml popup carries the implicit-height formula", async () => {
  assert.match(SELECT_QML, /popup: T\.Popup \{[\s\S]*?implicitHeight: contentHeight \+ topPadding \+ bottomPadding/);
});

test("emitQml 6.5: SpinBox.qml pads for the buttons and steps on wheel only when focused", async () => {
  assert.match(SPINBOX_QML, /rightPadding: 32/);
  // Steps by writing `value` directly: Qt 6.11 dropped the Q_INVOKABLE from
  // increase()/decrease() (QQuickAbstractSpinBox refactor) — calling them is a TypeError.
  assert.match(SPINBOX_QML, /WheelHandler \{[\s\S]*?acceptedDevices: PointerDevice\.Mouse \| PointerDevice\.TouchPad[\s\S]*?valueModified\(\)/);
  assert.doesNotMatch(SPINBOX_QML, /increase\(\)/);
});

// --- Tab navigation (desktop): every interactive widget is a tab stop, bound to the
// solidTabstop context property (loader-owned) so `tabstop.enabled = false` opts the
// whole app out at runtime. ---

const TAB_STOP = /activeFocusOnTab: solidTabstop\.enabled/;

test("tabstop: TextField.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  // The tab wiring moved into the component (to be reworked in the tabstop pass).
  assert.match(TEXTFIELD_QML, TAB_STOP);
});

test("tabstop: TextArea.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(TEXTAREA_QML, TAB_STOP);
});

test("tabstop: Checkbox.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(CHECKBOX_QML, TAB_STOP);
});

test("tabstop: Toggle.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(TOGGLE_QML, TAB_STOP);
});

test("tabstop: radio binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(await qml(`export function F(){ return <input type="radio" name="g" />; }`), TAB_STOP);
});

test("tabstop: Select.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(SELECT_QML, TAB_STOP);
});

test("tabstop: Slider.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(SLIDER_QML, TAB_STOP);
});

test("tabstop: SpinBox.qml binds activeFocusOnTab to solidTabstop.enabled", async () => {
  assert.match(SPINBOX_QML, TAB_STOP);
});

test("tabstop: SpinBox.qml forwards scope focus into the editable text", async () => {
  // T.SpinBox is a focus scope; without focus: true on the contentItem, tabbing into the
  // control leaves the TextInput unfocused and typing goes nowhere.
  assert.match(SPINBOX_QML, /contentItem: TextInput \{[\s\S]*?focus: true/);
});

test("tabstop: DateField.qml field is a tab stop and opens the popup from the keyboard", async () => {
  assert.match(DATEFIELD_QML, TAB_STOP);
  assert.match(DATEFIELD_QML, /Keys\.onReturnPressed/);
  assert.match(DATEFIELD_QML, /Keys\.onSpacePressed/);
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

test("calendar: MonthGrid.qml gets a Grid(7x6)+Repeater contentItem wired to source/delegate", async () => {
  assert.match(MONTHGRID_QML, /T\.AbstractMonthGrid \{[\s\S]*?contentItem: Grid \{[\s\S]*?columns: 7[\s\S]*?rows: 6[\s\S]*?Repeater \{[\s\S]*?model: mg\.source[\s\S]*?delegate: mg\.delegate/);
});

test("calendar: MonthGrid.qml DayOfWeekRow gets a Row+Repeater contentItem wired to source/delegate", async () => {
  assert.match(MONTHGRID_QML, /T\.AbstractDayOfWeekRow \{[\s\S]*?contentItem: Row \{[\s\S]*?Repeater \{[\s\S]*?model: dow\.source[\s\S]*?delegate: dow\.delegate/);
});

test("calendar: both Calendar and the date popup embed the shared MonthGrid", async () => {
  assert.match(CALENDAR_QML, /MonthGrid \{/);
  assert.match(DATEFIELD_QML, /contentItem: MonthGrid \{/);
});

test("calendar: MonthGrid.qml cells size declaratively (incubated creation misses the C++ resizeItems)", async () => {
  assert.match(MONTHGRID_QML, /delegate: Item \{[\s\S]*?width: \(dow\.contentItem\.width - 6 \* dow\.spacing\) \/ 7/);
  assert.match(MONTHGRID_QML, /T\.AbstractButton \{[\s\S]*?width: \(mg\.contentItem\.width - 6 \* mg\.spacing\) \/ 7[\s\S]*?height: \(mg\.contentItem\.height - 5 \* mg\.spacing\) \/ 6/);
});

// --- Toggles must SHOW tab focus: AbstractButton already takes tab focus in C++
// (setActiveFocusOnTab(true) + StrongFocus in its constructor), but without "focus"
// in cssState there is no visual feedback and the tab stop looks dead. ---

test("tabstop: Checkbox.qml cssState carries focus alongside checked/disabled", async () => {
  assert.match(CHECKBOX_QML, /\(box\.activeFocus \? \["focus"\] : \[\]\)\.concat\(box\.checked \? \["checked"\] : \[\]\)\.concat\(!box\.enabled \? \["disabled"\] : \[\]\)/);
});

test("tabstop: Toggle.qml cssState carries focus", async () => {
  assert.match(TOGGLE_QML, /\(sw\.activeFocus \? \["focus"\] : \[\]\)\.concat\(sw\.checked/);
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

test("select: Select.qml popup background and contentItem re-anchor the CSS chain at the control", async () => {
  assert.match(SELECT_QML, /popup: T\.Popup \{[\s\S]*?background: Css\.CssFill \{[\s\S]*?property Item cssAncestor: ctl/);
  assert.match(SELECT_QML, /contentItem: ListView \{[\s\S]*?property Item cssAncestor: ctl/);
});

test("date: DateField.qml popup background and contentItem re-anchor the CSS chain at the wrapper", async () => {
  assert.match(DATEFIELD_QML, /T\.Popup \{[\s\S]*?background: Css\.CssFill \{[\s\S]*?property Item cssAncestor: root/);
  assert.match(DATEFIELD_QML, /T\.Popup \{[\s\S]*?contentItem: MonthGrid \{[\s\S]*?property Item cssAncestor: root/);
});

// --- Arrow keys (desktop): radios navigate AND check within their ButtonGroup (HTML/desktop
// semantics); checkbox/switch arrows move focus along the tab chain (dialog semantics),
// gated by the tabstop opt-out. Slider gains the same focused-wheel stepping as SpinBox. ---

test("arrows: radio moves SELECTION through the group (checks + focuses, wraps) — §6", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  // §6: arrows move the selection (no focused-but-unchecked radio) — __step CHECKS the target then
  // focuses it, wrapping via modulo over the group's declaration order.
  assert.match(out, /function __step\(d\) \{ var bs = __group_g\.order;[\s\S]*?bs\[j\]\.checked = true; bs\[j\]\.forceActiveFocus\(Qt\.TabFocusReason\) \}/);
  assert.match(out, /Keys\.onDownPressed: __step\(1\)/);
  assert.match(out, /Keys\.onRightPressed: __step\(1\)/);
  assert.match(out, /Keys\.onUpPressed: __step\(-1\)/);
  assert.match(out, /Keys\.onLeftPressed: __step\(-1\)/);
});

test("arrows: the ButtonGroup carries the radios in declaration order", async () => {
  const out = await qmlType(`
    export function F() {
      return <div>
        <input type="radio" name="g" />
        <input type="radio" name="g" />
        <input type="radio" name="g" />
      </div>;
    }
  `);
  assert.match(out, /T\.ButtonGroup \{ id: __group_g; readonly property var order: \[__input0, __input1, __input2\] \}/);
});

test("arrows: radio without a name emits no arrow handlers", async () => {
  const out = await qml(`export function F(){ return <input type="radio" />; }`);
  assert.doesNotMatch(out, /Keys\.onDownPressed/);
});

test("focus: Checkbox.qml and Toggle.qml are single tab stops — arrows do NOT move focus (study §6)", async () => {
  // Desktop model: a checkbox/switch is one tab stop (Tab between, Space toggles). Arrows moving
  // focus was the "arrows leak focus out of the group" bug — only a radio GROUP arrow-navigates.
  for (const qmlSrc of [CHECKBOX_QML, TOGGLE_QML]) {
    assert.match(qmlSrc, /activeFocusOnTab: solidTabstop\.enabled/);
    assert.doesNotMatch(qmlSrc, /nextItemInFocusChain/);
  }
});

test("wheel: Slider.qml steps value when focused and re-emits moved()", async () => {
  assert.match(SLIDER_QML, /WheelHandler \{[\s\S]*?acceptedDevices: PointerDevice\.Mouse \| PointerDevice\.TouchPad[\s\S]*?ctl\.moved\(\)/);
});

test("date: DateField.qml chevron is the same CssText as the select's, anchored right inside the host Item", async () => {
  assert.match(DATEFIELD_QML, /Css\.CssText \{\n\s*cssPrimitive: ""\n\s*cssClass: \["chevron"\]\n\s*text: "▾"\n\s*anchors\.right: parent\.right\n\s*anchors\.rightMargin: 8/);
});

test("calendar: MonthGrid.qml day label carries the day states (sibling slots — no ancestor relation)", async () => {
  // Both background and contentItem reference the shared __dayState (today/selected/outside/hover/focus).
  assert.match(MONTHGRID_QML, /cssClass: \["day-label"\]\n\s*cssState: mgDel\.__dayState/);
  assert.match(MONTHGRID_QML, /readonly property var __dayState: \(model\.today \? \["today"\] : \[\]\)/);
});

// --- Desktop popups: combo/date dropdowns are REAL windows (popupType: Popup.Window,
// Templates 6.8+) so they escape the app window, and they flip ABOVE the control when
// opening below would overflow the screen. ---

test("popups: Select.qml popup is an in-scene item popup and flips above on window overflow", async () => {
  assert.match(SELECT_QML, /popup: T\.Popup \{[\s\S]*?popupType: T\.Popup\.Item/);
  assert.match(SELECT_QML, /y: \(ctl\.mapToItem\(null, 0, ctl\.height \+ 2\)\.y \+ height > \(ctl\.Window\.height \|\| Screen\.height\)\) \? -\(height \+ 2\) : ctl\.height \+ 2/);
});

test("popups: DateField.qml popup is an in-scene item popup and flips above on window overflow", async () => {
  assert.match(DATEFIELD_QML, /T\.Popup \{[\s\S]*?popupType: T\.Popup\.Item/);
  assert.match(DATEFIELD_QML, /y: \(root\.mapToItem\(null, 0, root\.height \+ 2\)\.y \+ height > \(root\.Window\.height \|\| Screen\.height\)\) \? -\(height \+ 2\) : root\.height \+ 2/);
});

test("popups: DateField.qml uses the Item popupType (6.8 Templates import owned by the .qml)", async () => {
  // The Templates version lives in DateField.qml now; the generated component imports Widgets only.
  assert.match(DATEFIELD_QML, /popupType: T\.Popup\.Item/);
});

test("select: Select.qml delegate binds highlighted to the combo's highlightedIndex (keyboard nav visible)", async () => {
  assert.match(SELECT_QML, /T\.ItemDelegate \{[\s\S]*?highlighted: ctl\.highlightedIndex === index/);
});

test("date: DateField.qml field click after a press-outside close does not reopen (toggle race)", async () => {
  assert.match(DATEFIELD_QML, /property double __closedAt: 0/);
  assert.match(DATEFIELD_QML, /onClosed: \{ __closedAt = Date\.now\(\)/);
  assert.match(DATEFIELD_QML, /onClicked: \{ field\.forceActiveFocus\(\); if \(pop\.visible\) pop\.close\(\); else if \(Date\.now\(\) - pop\.__closedAt > 150\) pop\.open\(\) \}/);
});

// --- Date popup keyboard: arrows move a day cursor (±1 / ±7), Enter/Space commit it
// through the same onChange path a cell click uses; Down opens the closed popup. The
// popup keeps focus on the field, so the Keys live there. ---

test("date keyboard: DateField.qml field arrows step the cursor and Enter commits", async () => {
  assert.match(DATEFIELD_QML, /function __step\(days\)/);
  assert.match(DATEFIELD_QML, /Keys\.onDownPressed: pop\.visible \? root\.__step\(7\) : pop\.open\(\)/);
  assert.match(DATEFIELD_QML, /Keys\.onLeftPressed: \{ if \(pop\.visible\) root\.__step\(-1\) \}/);
  assert.match(DATEFIELD_QML, /Keys\.onReturnPressed: pop\.visible \? root\.__commit\(\) : pop\.open\(\)/);
});

test("date keyboard: DateField.qml popup inits the cursor on open, clears on close; MonthGrid cell shows :focus", async () => {
  assert.match(DATEFIELD_QML, /onOpened: root\.cursor = root\.value instanceof Date \? root\.value : new Date\(\)/);
  assert.match(DATEFIELD_QML, /onClosed: \{ __closedAt = Date\.now\(\); root\.cursor = null \}/);
  // The :focus concat rides the shared cursorDate (DateField passes root.cursor into the grid).
  assert.match(DATEFIELD_QML, /cursorDate: root\.cursor/);
  assert.match(MONTHGRID_QML, /\.concat\(\(root\.cursorDate instanceof Date[\s\S]*?\) \? \["focus"\] : \[\]\)/);
});

test("date keyboard: Enter commit routes the cursor date through onDayPicked (the author's onChange)", async () => {
  const out = await qmlType(`
    export function F() {
      const [d, setD] = createSignal(null);
      return <input type="date" value={d()} onChange={(e) => setD(e.target.valueAsDate)} />;
    }
  `);
  // DateField.qml's __commit fires dayPicked(cursor) then closes; the emit's onDayPicked runs the body.
  assert.match(DATEFIELD_QML, /function __commit\(\) \{[\s\S]*?root\.dayPicked\(cursor\)[\s\S]*?pop\.close\(\)/);
  assert.match(out, /onDayPicked: \(date\) => \{ d = date \}/);
});

test("date keyboard: DateField.qml field click gives the field focus (keyboard works after mouse open)", async () => {
  assert.match(DATEFIELD_QML, /onClicked: \{ field\.forceActiveFocus\(\);/);
});

test("popups: dropdowns close when the app window deactivates (Qt::Popup semantics)", async () => {
  assert.match(SELECT_QML, /Window\.onActiveChanged: if \(!Window\.active\) ctl\.popup\.close\(\)/);
  assert.match(DATEFIELD_QML, /Window\.onActiveChanged: if \(!Window\.active\) pop\.close\(\)/);
});

test("tabstop: a radio group is ONE tab stop — checked radio (or first) only", async () => {
  const out = await qml(`export function F(){ return <input type="radio" name="g" />; }`);
  assert.match(out, /activeFocusOnTab: solidTabstop\.enabled && \(__input0\.checked \|\| \(!__group_g\.checkedButton && __group_g\.buttons\.length > 0 && __group_g\.buttons\[0\] === __input0\)\)/);
  const bare = await qml(`export function F(){ return <input type="radio" />; }`);
  assert.match(bare, /activeFocusOnTab: solidTabstop\.enabled\n/);
});
