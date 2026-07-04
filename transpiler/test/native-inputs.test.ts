/** Native-only input widgets (inputs group): <RangeSlider>, <Dial>, <Tumbler>, <DelayButton>,
 *  <BusyIndicator>, <RoundButton>, <ToolButton>, <ToolSeparator>.
 *
 *  Helpers mirror test/widgets.test.ts: `qml()` for direct emitQml calls, `qmlType()` for full
 *  emitComponentType output (needed for the Templates import check). */

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
// <RangeSlider>
// ---------------------------------------------------------------------------

test("native-inputs: <RangeSlider> emits wrapper CssFill + T.RangeSlider with from/to/stepSize", async () => {
  const out = await qml(`export function F(){ return <RangeSlider min={10} max={90} step={5} />; }`);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssPrimitive: "input"/);
  assert.match(out, /T\.RangeSlider \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /from: 10/);
  assert.match(out, /to: 90/);
  assert.match(out, /stepSize: 5/);
});

test("native-inputs: <RangeSlider> defaults min/max/step to 0/100/1", async () => {
  const out = await qml(`export function F(){ return <RangeSlider />; }`);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 100/);
  assert.match(out, /stepSize: 1/);
});

test("native-inputs: <RangeSlider> emits track, range fill and TWO handles", async () => {
  const out = await qml(`export function F(){ return <RangeSlider />; }`);
  assert.match(out, /cssClass: \["track"\]/);
  // range fill spans first→second visualPosition, hosted in an anchored Item
  assert.match(out, /cssClass: \["track-fill"\]/);
  assert.match(out, /x: __input0\.first\.visualPosition \* parent\.width/);
  assert.match(out, /width: \(__input0\.second\.visualPosition - __input0\.first\.visualPosition\) \* parent\.width/);
  // both handle slots, emitSlider's 18×18 geometry
  assert.match(out, /first\.handle: Css\.CssRect \{/);
  assert.match(out, /second\.handle: Css\.CssRect \{/);
  assert.match(out, /x: __input0\.leftPadding \+ __input0\.first\.visualPosition \* \(__input0\.availableWidth - width\)/);
  assert.match(out, /x: __input0\.leftPadding \+ __input0\.second\.visualPosition \* \(__input0\.availableWidth - width\)/);
  const handles = out.match(/cssClass: \["handle"\]/g) ?? [];
  assert.equal(handles.length, 2);
});

test("native-inputs: <RangeSlider first second> emits two Binding elements on the sub-nodes", async () => {
  const out = await qmlType(`
    export function F() {
      const [lo, setLo] = createSignal(20);
      const [hi, setHi] = createSignal(80);
      return <RangeSlider first={lo()} second={hi()} />;
    }
  `);
  assert.match(out, /target: __input0\.first/);
  assert.match(out, /target: __input0\.second/);
  assert.match(out, /property: "value"/);
  assert.match(out, /value: lo/);
  assert.match(out, /value: hi/);
  const restores = out.match(/restoreMode: Binding\.RestoreNone/g) ?? [];
  assert.equal(restores.length, 2);
});

test("native-inputs: <RangeSlider onChange> fires from first.onMoved AND second.onMoved with both values", async () => {
  const out = await qmlType(`
    export function F() {
      const [lo, setLo] = createSignal(20);
      const [hi, setHi] = createSignal(80);
      return <RangeSlider first={lo()} second={hi()} onChange={(a, b) => { setLo(a); setHi(b); }} />;
    }
  `);
  assert.match(out, /first\.onMoved: \{ __self\.lo = __input0\.first\.value; __self\.hi = __input0\.second\.value; \}/);
  assert.match(out, /second\.onMoved: \{ __self\.lo = __input0\.first\.value; __self\.hi = __input0\.second\.value; \}/);
});

test("native-inputs: <RangeSlider> focused wheel steps the FIRST handle (accumulator, Mouse|TouchPad)", async () => {
  const out = await qml(`export function F(){ return <RangeSlider />; }`);
  assert.match(out, /WheelHandler \{/);
  assert.match(out, /enabled: __input0\.activeFocus/);
  assert.match(out, /acceptedDevices: PointerDevice\.Mouse \| PointerDevice\.TouchPad/);
  assert.match(out, /__input0\.first\.value = Math\.max\(__input0\.from, Math\.min\(__input0\.to, __input0\.first\.value \+ s \* __input0\.stepSize\)\)/);
  assert.match(out, /__input0\.first\.moved\(\)/);
});

test("native-inputs: <RangeSlider> is a tab stop and carries focus/disabled cssState", async () => {
  const out = await qml(`export function F(){ return <RangeSlider />; }`);
  assert.match(out, /activeFocusOnTab: solidTabstop\.enabled/);
  assert.match(out, /cssState: \(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
});

test("native-inputs: <RangeSlider> emits the Templates import in the component type", async () => {
  const out = await qmlType(`export function F(){ return <RangeSlider />; }`);
  assert.match(out, /import QtQuick\.Templates 6\.0 as T/);
});

// ---------------------------------------------------------------------------
// <Dial>
// ---------------------------------------------------------------------------

test("native-inputs: <Dial> emits T.Dial with from/to/stepSize and dial background", async () => {
  const out = await qml(`export function F(){ return <Dial min={0} max={360} step={10} />; }`);
  assert.match(out, /T\.Dial \{/);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 360/);
  assert.match(out, /stepSize: 10/);
  assert.match(out, /background: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["dial"\]/);
});

test("native-inputs: <Dial> handle is a 12×12 CssRect positioned from dial.angle trig", async () => {
  const out = await qml(`export function F(){ return <Dial />; }`);
  assert.match(out, /handle: Css\.CssRect \{/);
  assert.match(out, /cssClass: \["handle"\]/);
  assert.match(out, /width: 12/);
  assert.match(out, /height: 12/);
  assert.match(out, /Math\.sin\(__input0\.angle \* Math\.PI \/ 180\) \* \(__input0\.background\.width \/ 2 - 12\)/);
  assert.match(out, /Math\.cos\(__input0\.angle \* Math\.PI \/ 180\) \* \(__input0\.background\.width \/ 2 - 12\)/);
});

test("native-inputs: <Dial value> emits a Binding on value; onChange={(v)=>…} maps v to dial.value via onMoved", async () => {
  const out = await qmlType(`
    export function F() {
      const [a, setA] = createSignal(30);
      return <Dial value={a()} onChange={(v) => setA(v)} />;
    }
  `);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __input0/);
  assert.match(out, /property: "value"/);
  assert.match(out, /value: a/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
  assert.match(out, /onMoved: \{ __self\.a = __input0\.value \}/);
});

// ---------------------------------------------------------------------------
// <Tumbler>
// ---------------------------------------------------------------------------

test("native-inputs: <Tumbler options> emits T.Tumbler with the options model and explicit wrap:false", async () => {
  const out = await qml(`export function F(){ return <Tumbler options={["S", "M", "L", "XL"]} />; }`);
  assert.match(out, /T\.Tumbler \{/);
  assert.match(out, /model: \["S", "M", "L", "XL"\]/);
  assert.match(out, /wrap: false/);
});

test("native-inputs: <Tumbler> contentItem is the minimal non-wrap ListView (snap + strict highlight range)", async () => {
  const out = await qml(`export function F(){ return <Tumbler options={["a"]} />; }`);
  assert.match(out, /contentItem: ListView \{/);
  assert.match(out, /snapMode: ListView\.SnapToItem/);
  assert.match(out, /highlightRangeMode: ListView\.StrictlyEnforceRange/);
  assert.match(out, /preferredHighlightBegin: height \/ 2 - height \/ __input0\.visibleItemCount \/ 2/);
  assert.match(out, /preferredHighlightEnd: height \/ 2 \+ height \/ __input0\.visibleItemCount \/ 2/);
});

test("native-inputs: <Tumbler> delegate reads Tumbler.displacement on its root and marks the settled row selected", async () => {
  const out = await qml(`export function F(){ return <Tumbler options={["a"]} />; }`);
  assert.match(out, /delegate: Item \{/);
  assert.match(out, /property real __disp: T\.Tumbler\.displacement/);
  assert.match(out, /cssClass: \["item"\]/);
  assert.match(out, /cssState: Math\.abs\(__disp\) < 0\.5 \? \["selected"\] : \[\]/);
  // declarative cell size — the template's own resize formula, incubation-proof
  assert.match(out, /width: __input0\.availableWidth/);
  assert.match(out, /height: __input0\.availableHeight \/ __input0\.visibleItemCount/);
});

test("native-inputs: <Tumbler value> binds currentIndex via options.indexOf(value)", async () => {
  const out = await qmlType(`
    export function F() {
      const [size, setSize] = createSignal("M");
      return <Tumbler options={["S", "M"]} value={size()} />;
    }
  `);
  assert.match(out, /property: "currentIndex"/);
  assert.match(out, /value: \(\["S", "M"\]\)\.indexOf\(size\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("native-inputs: <Tumbler onChange> fires from onCurrentIndexChanged with the picked model value", async () => {
  const out = await qmlType(`
    export function F() {
      const [size, setSize] = createSignal("M");
      return <Tumbler options={["S", "M"]} value={size()} onChange={(v) => setSize(v)} />;
    }
  `);
  assert.match(out, /onCurrentIndexChanged: \{ __self\.size = __input0\.model\[__input0\.currentIndex\] \}/);
});

// ---------------------------------------------------------------------------
// <DelayButton>
// ---------------------------------------------------------------------------

test("native-inputs: <DelayButton delay> emits T.DelayButton with the delay and the .delay background", async () => {
  const out = await qml(`export function F(){ return <DelayButton delay={1200}>Hold</DelayButton>; }`);
  assert.match(out, /T\.DelayButton \{/);
  assert.match(out, /delay: 1200/);
  assert.match(out, /cssClass: \["delay"\]/);
});

test("native-inputs: <DelayButton> progress overlay is a CssRect growing with progress", async () => {
  const out = await qml(`export function F(){ return <DelayButton>Hold</DelayButton>; }`);
  assert.match(out, /cssClass: \["delay-fill"\]/);
  assert.match(out, /width: __input0\.progress \* parent\.width/);
});

test("native-inputs: <DelayButton> label comes from the text child via CssText", async () => {
  const out = await qml(`export function F(){ return <DelayButton>Hold to arm</DelayButton>; }`);
  assert.match(out, /contentItem: Css\.CssText \{/);
  assert.match(out, /text: "Hold to arm"/);
});

test("native-inputs: <DelayButton onActivated> maps to onActivated", async () => {
  const out = await qmlType(`
    export function F() {
      const [armed, setArmed] = createSignal(false);
      return <DelayButton delay={500} onActivated={() => setArmed(true)}>Hold</DelayButton>;
    }
  `);
  assert.match(out, /onActivated: \{ __self\.armed = true \}/);
});

test("native-inputs: <DelayButton> carries the full button state list", async () => {
  const out = await qml(`export function F(){ return <DelayButton>Hold</DelayButton>; }`);
  assert.match(out, /cssState: \(__input0\.hovered \? \["hover"\] : \[\]\)\.concat\(__input0\.pressed \? \["active"\] : \[\]\)\.concat\(__input0\.checked \? \["checked"\] : \[\]\)/);
});

// ---------------------------------------------------------------------------
// <BusyIndicator>
// ---------------------------------------------------------------------------

test("native-inputs: <BusyIndicator running> wires running and hides when stopped", async () => {
  const out = await qmlType(`
    export function F() {
      const [busy, setBusy] = createSignal(true);
      return <BusyIndicator running={busy()} />;
    }
  `);
  assert.match(out, /T\.BusyIndicator \{/);
  assert.match(out, /running: busy/);
  assert.match(out, /visible: running/);
});

test("native-inputs: <BusyIndicator> spins 8 staggered spokes on an Item host", async () => {
  const out = await qml(`export function F(){ return <BusyIndicator />; }`);
  assert.match(out, /contentItem: Item \{/);
  assert.match(out, /RotationAnimation on rotation \{/);
  assert.match(out, /duration: 900/);
  assert.match(out, /loops: Animation\.Infinite/);
  const spokes = out.match(/cssClass: \["spoke"\]/g) ?? [];
  assert.equal(spokes.length, 8);
  const rects = out.match(/^\s*Rectangle \{/gm) ?? [];
  assert.equal(rects.length, 8);
  // circle placement + opacity stagger
  assert.match(out, /Math\.cos\(3 \* Math\.PI \/ 4\)/);
  assert.match(out, /opacity: 0\.125/);
  assert.match(out, /opacity: 1/);
});

test("native-inputs: <BusyIndicator> animation is gated on the author's running expression", async () => {
  const out = await qmlType(`
    export function F() {
      const [busy, setBusy] = createSignal(false);
      return <BusyIndicator running={busy()} />;
    }
  `);
  assert.match(out, /RotationAnimation on rotation \{[\s\S]*?running: busy[\s\S]*?\}/);
});

// ---------------------------------------------------------------------------
// <RoundButton> / <ToolButton>
// ---------------------------------------------------------------------------

test("native-inputs: <RoundButton> is a button-primitive CssFill with the extra 'round' class", async () => {
  const out = await qml(`export function F(){ return <RoundButton class="fab">+1</RoundButton>; }`);
  assert.match(out, /cssClass: \["fab", "round"\]/);
  assert.match(out, /cssPrimitive: "button"/);
  assert.match(out, /T\.RoundButton \{/);
  assert.match(out, /background: null/);
  assert.match(out, /text: "\+1"/);
});

test("native-inputs: <RoundButton onClick> maps to onClicked", async () => {
  const out = await qmlType(`
    export function F() {
      const [n, setN] = createSignal(0);
      return <RoundButton onClick={() => setN(n() + 1)}>+</RoundButton>;
    }
  `);
  assert.match(out, /onClicked: \{ __self\.n = n \+ 1 \}/);
});

test("native-inputs: <ToolButton> mirrors RoundButton with the 'tool' class", async () => {
  const out = await qml(`export function F(){ return <ToolButton>reset</ToolButton>; }`);
  assert.match(out, /cssClass: \["tool"\]/);
  assert.match(out, /T\.ToolButton \{/);
  assert.match(out, /text: "reset"/);
});

test("native-inputs: Round/Tool buttons carry hover/active/focus/disabled state and tab stop", async () => {
  const out = await qml(`export function F(){ return <RoundButton>x</RoundButton>; }`);
  assert.match(out, /cssState: \(__input0\.hovered \? \["hover"\] : \[\]\)\.concat\(__input0\.pressed \? \["active"\] : \[\]\)\.concat\(__input0\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__input0\.enabled \? \["disabled"\] : \[\]\)/);
  assert.match(out, /activeFocusOnTab: solidTabstop\.enabled/);
});

// ---------------------------------------------------------------------------
// <ToolSeparator>
// ---------------------------------------------------------------------------

test("native-inputs: <ToolSeparator> emits T.ToolSeparator with a centred 1px × 60% CssRect", async () => {
  const out = await qml(`export function F(){ return <ToolSeparator />; }`);
  assert.match(out, /T\.ToolSeparator \{/);
  assert.match(out, /cssClass: \["sep"\]/);
  assert.match(out, /width: 1/);
  assert.match(out, /height: parent\.height \* 0\.6/);
  assert.match(out, /anchors\.horizontalCenter: parent\.horizontalCenter/);
  assert.match(out, /anchors\.verticalCenter: parent\.verticalCenter/);
});

// ---------------------------------------------------------------------------
// Shared behaviour
// ---------------------------------------------------------------------------

test("native-inputs: multiple widgets in one tree get distinct control ids", async () => {
  const out = await qml(`export function F(){ return (
    <div>
      <RangeSlider />
      <Dial />
      <Tumbler options={["a"]} />
    </div>
  ); }`);
  assert.match(out, /id: __input0/);
  assert.match(out, /id: __input1/);
  assert.match(out, /id: __input2/);
});

test("native-inputs: guard (Show) gates the wrapper visibility", async () => {
  const out = await qmlType(`
    export function F() {
      const [open, setOpen] = createSignal(true);
      return <div><Show when={open()}><Dial /></Show></div>;
    }
  `);
  assert.match(out, /visible: !!\(open\)/);
});
