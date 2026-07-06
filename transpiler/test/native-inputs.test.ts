/** Native-only input widgets (inputs group): <RangeSlider>, <Dial>, <Tumbler>, <DelayButton>,
 *  <BusyIndicator>, <RoundButton>, <ToolButton>, <ToolSeparator>.
 *
 *  Helpers mirror test/widgets.test.ts: `qml()` for direct emitQml calls, `qmlType()` for full
 *  emitComponentType output (needed for the Templates import check). */

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

/** Read a migrated component's .qml source from the solidqml.Widgets module. */
function readWidget(name: string): Promise<string> {
  return readFile(fileURLToPath(new URL(`../../qml/solidqml/Widgets/${name}.qml`, import.meta.url)), "utf8");
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

test("native-inputs: <Dial> instantiates W.Dial with from/to/stepSize on the instance", async () => {
  const out = await qml(`export function F(){ return <Dial min={0} max={360} step={10} />; }`);
  assert.match(out, /W\.Dial \{/);
  assert.match(out, /id: __input0/);
  assert.match(out, /from: 0/);
  assert.match(out, /to: 360/);
  assert.match(out, /stepSize: 10/);
  // The T.Dial + face + handle now live in Dial.qml, not the emit.
  assert.doesNotMatch(out, /T\.Dial/);
});

test("native-inputs: Dial.qml holds the T.Dial, dial face and trig-placed handle", async () => {
  const src = await readWidget("Dial");
  assert.match(src, /T\.Dial \{/);
  assert.match(src, /background: Css\.CssFill \{/);
  assert.match(src, /cssClass: \["dial"\]/);
  assert.match(src, /handle: Css\.CssRect \{/);
  assert.match(src, /cssClass: \["handle"\]/);
  assert.match(src, /Math\.sin\(__ctl\.angle \* Math\.PI \/ 180\) \* \(__ctl\.background\.width \/ 2 - 12\)/);
  assert.match(src, /Math\.cos\(__ctl\.angle \* Math\.PI \/ 180\) \* \(__ctl\.background\.width \/ 2 - 12\)/);
  assert.match(src, /property alias value: __ctl\.value/);
});

test("native-inputs: <Dial value> emits a Binding on value; onChange={(v)=>…} maps v to __input0.value via onMoved", async () => {
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
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
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

test("native-inputs: <DelayButton delay> instantiates W.DelayButton with delay + label text", async () => {
  const out = await qml(`export function F(){ return <DelayButton delay={1200}>Hold</DelayButton>; }`);
  assert.match(out, /W\.DelayButton \{/);
  assert.match(out, /delay: 1200/);
  assert.match(out, /text: "Hold"/);
  // The T.DelayButton + .delay pill now live in DelayButton.qml, not the emit.
  assert.doesNotMatch(out, /T\.DelayButton/);
});

test("native-inputs: <DelayButton> label comes from the text child", async () => {
  const out = await qml(`export function F(){ return <DelayButton>Hold to arm</DelayButton>; }`);
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
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
});

test("native-inputs: DelayButton.qml holds the pill background, progress overlay and state list", async () => {
  const src = await readWidget("DelayButton");
  assert.match(src, /T\.DelayButton \{/);
  assert.match(src, /cssClass: \["delay"\]/);
  assert.match(src, /cssClass: \["delay-fill"\]/);
  assert.match(src, /width: __ctl\.progress \* parent\.width/);
  assert.match(src, /contentItem: Css\.CssText \{/);
  assert.match(src, /cssState: \(__ctl\.hovered \? \["hover"\] : \[\]\)\.concat\(__ctl\.pressed \? \["active"\] : \[\]\)\.concat\(__ctl\.checked \? \["checked"\] : \[\]\)/);
});

// ---------------------------------------------------------------------------
// <BusyIndicator>
// ---------------------------------------------------------------------------

test("native-inputs: <BusyIndicator running> instantiates W.BusyIndicator and forwards running", async () => {
  const out = await qmlType(`
    export function F() {
      const [busy, setBusy] = createSignal(true);
      return <BusyIndicator running={busy()} />;
    }
  `);
  assert.match(out, /W\.BusyIndicator \{/);
  assert.match(out, /running: busy/);
  // The T.BusyIndicator + spokes now live in BusyIndicator.qml, not the emit.
  assert.doesNotMatch(out, /T\.BusyIndicator/);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
});

test("native-inputs: <BusyIndicator> with no running attr omits the prop (component default true)", async () => {
  const out = await qml(`export function F(){ return <BusyIndicator />; }`);
  assert.match(out, /W\.BusyIndicator \{/);
  assert.doesNotMatch(out, /running:/);
});

test("native-inputs: BusyIndicator.qml spins 8 staggered spokes gated on running", async () => {
  const src = await readWidget("BusyIndicator");
  assert.match(src, /T\.BusyIndicator \{/);
  assert.match(src, /contentItem: Item \{/);
  assert.match(src, /RotationAnimation on rotation \{/);
  assert.match(src, /duration: 900/);
  assert.match(src, /loops: Animation\.Infinite/);
  assert.match(src, /running: root\.running/);
  const spokes = src.match(/cssClass: \["spoke"\]/g) ?? [];
  assert.equal(spokes.length, 8);
  const rects = src.match(/^\s*Rectangle \{/gm) ?? [];
  assert.equal(rects.length, 8);
  assert.match(src, /Math\.cos\(3 \* Math\.PI \/ 4\)/);
  assert.match(src, /opacity: 0\.125/);
  assert.match(src, /opacity: 1/);
  assert.match(src, /visible: running/);
});

// ---------------------------------------------------------------------------
// <RoundButton> / <ToolButton>
// ---------------------------------------------------------------------------

test("native-inputs: <RoundButton> instantiates W.RoundButton with the extra 'round' class + text", async () => {
  const out = await qml(`export function F(){ return <RoundButton class="fab">+1</RoundButton>; }`);
  assert.match(out, /W\.RoundButton \{/);
  assert.match(out, /cssClass: \["fab", "round"\]/);
  assert.match(out, /text: "\+1"/);
  // The T.RoundButton + label internals now live in RoundButton.qml, not the emit.
  assert.doesNotMatch(out, /T\.RoundButton/);
});

test("native-inputs: <RoundButton onClick> maps to onClicked", async () => {
  const out = await qmlType(`
    export function F() {
      const [n, setN] = createSignal(0);
      return <RoundButton onClick={() => setN(n() + 1)}>+</RoundButton>;
    }
  `);
  assert.match(out, /onClicked: \{ __self\.n = n \+ 1 \}/);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
});

test("native-inputs: <ToolButton> mirrors RoundButton with the 'tool' class", async () => {
  const out = await qml(`export function F(){ return <ToolButton>reset</ToolButton>; }`);
  assert.match(out, /W\.ToolButton \{/);
  assert.match(out, /cssClass: \["tool"\]/);
  assert.match(out, /text: "reset"/);
});

test("native-inputs: RoundButton.qml / ToolButton.qml carry the button primitive + state list", async () => {
  const round = await readWidget("RoundButton");
  assert.match(round, /T\.RoundButton \{/);
  assert.match(round, /cssPrimitive: "button"/);
  assert.match(round, /background: null/);
  assert.match(round, /cssState: \(__ctl\.hovered \? \["hover"\] : \[\]\)\.concat\(__ctl\.pressed \? \["active"\] : \[\]\)\.concat\(__ctl\.activeFocus \? \["focus"\] : \[\]\)\.concat\(!__ctl\.enabled \? \["disabled"\] : \[\]\)/);
  assert.match(round, /activeFocusOnTab: solidTabstop\.enabled/);
  const tool = await readWidget("ToolButton");
  assert.match(tool, /T\.ToolButton \{/);
  assert.match(tool, /cssPrimitive: "button"/);
});

// ---------------------------------------------------------------------------
// <ToolSeparator>
// ---------------------------------------------------------------------------

test("native-inputs: <ToolSeparator> instantiates the W.ToolSeparator component", async () => {
  const out = await qml(`export function F(){ return <ToolSeparator class="sep-x" />; }`);
  assert.match(out, /W\.ToolSeparator \{/);
  assert.match(out, /cssClass: \["sep-x"\]/);
  // The T.ToolSeparator + centred "sep" CssRect now live in ToolSeparator.qml, not the emit.
  assert.doesNotMatch(out, /T\.ToolSeparator/);
});

test("native-inputs: emitting <ToolSeparator> imports the solidqml.Widgets module", async () => {
  const out = await qmlType(`export function F(){ return <ToolSeparator />; }`);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
});

test("native-inputs: ToolSeparator.qml holds the T.ToolSeparator + centred sep CssRect", async () => {
  const src = await readWidget("ToolSeparator");
  assert.match(src, /T\.ToolSeparator \{/);
  assert.match(src, /cssClass: \["sep"\]/);
  assert.match(src, /width: 1/);
  assert.match(src, /height: parent\.height \* 0\.6/);
  assert.match(src, /anchors\.horizontalCenter: parent\.horizontalCenter/);
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
