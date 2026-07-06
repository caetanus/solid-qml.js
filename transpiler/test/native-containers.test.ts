/** Native container tags: <ToolBar>, <TabBar>/<TabButton>, <SplitView>, <Drawer>,
 *  <StackView>, <SwipeView>/<PageIndicator> (plan: 2026-07-03-native-only-widgets).
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
import { generate } from "../src/index.ts";
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
// <ToolBar>
// ---------------------------------------------------------------------------

test("containers: <ToolBar> instantiates the W.ToolBar component with its class", async () => {
  const out = await qml(`export function F(){ return <ToolBar class="tb"><button>A</button></ToolBar>; }`);
  assert.match(out, /W\.ToolBar \{/);
  assert.match(out, /cssClass: \["tb"\]/);
  // The T.ToolBar chrome now lives in ToolBar.qml, not the emit.
  assert.doesNotMatch(out, /T\.ToolBar/);
});

test("containers: <ToolBar> children emit into the component's default content slot", async () => {
  const out = await qml(`export function F(){ return <ToolBar><button>Run</button></ToolBar>; }`);
  // The button (W.Button) emits as a child of the W.ToolBar instance.
  const toolbar = out.indexOf("W.ToolBar {");
  const button = out.indexOf("W.Button {");
  assert.ok(toolbar >= 0 && button > toolbar, "button must be a child of W.ToolBar");
  assert.match(out, /text: "Run"/);
});

test("containers: ToolBar.qml component hosts the T.ToolBar chrome internals", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/ToolBar.qml", import.meta.url)), "utf8");
  assert.match(src, /T\.ToolBar \{/);
  assert.match(src, /cssPrimitive: "toolbar"/);
  assert.match(src, /background: null/);
  assert.match(src, /contentItem: Item \{ \}/);
});

test("containers: <ToolBar> under <Show> carries the visible guard", async () => {
  const out = await qml(`export function F(){ const [shown, setShown] = createSignal(true); return <Show when={shown()}><ToolBar /></Show>; }`);
  assert.match(out, /visible: !!\(shown\)/);
});

// ---------------------------------------------------------------------------
// <TabBar> / <TabButton>
// ---------------------------------------------------------------------------

const TABBAR = `
  export function F() {
    const [tab, setTab] = createSignal(0);
    return (
      <TabBar class="tabs" current={tab()} onChange={(i) => setTab(i)}>
        <TabButton>One</TabButton>
        <TabButton>Two</TabButton>
      </TabBar>
    );
  }
`;

test("containers: <TabBar> instantiates W.TabBar keeping the __tabbar0 id", async () => {
  const out = await qml(TABBAR);
  assert.match(out, /W\.TabBar \{/);
  assert.match(out, /id: __tabbar0/);
  assert.match(out, /cssClass: \["tabs"\]/);
  // The T.TabBar + ListView contentItem now live in TabBar.qml.
  assert.doesNotMatch(out, /T\.TabBar/);
  assert.doesNotMatch(out, /contentItem: ListView/);
});

test("containers: <TabBar current> emits a RestoreNone Binding on currentIndex", async () => {
  const out = await qml(TABBAR);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __tabbar0/);
  assert.match(out, /property: "currentIndex"/);
  assert.match(out, /value: tab/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("containers: <TabBar onChange> fires with the control's currentIndex", async () => {
  const out = await qml(TABBAR);
  assert.match(out, /onCurrentIndexChanged: \{ tab = __tabbar0\.currentIndex \}/);
});

test("containers: <TabButton> children become W.TabButton with their label text", async () => {
  const out = await qml(TABBAR);
  assert.match(out, /W\.TabButton \{/);
  assert.match(out, /text: "One"/);
  assert.match(out, /text: "Two"/);
  // The T.TabButton + Css tab slots now live in TabButton.qml.
  assert.doesNotMatch(out, /T\.TabButton/);
});

test("containers: TabBar.qml + TabButton.qml host the control internals", async () => {
  const bar = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/TabBar.qml", import.meta.url)), "utf8");
  assert.match(bar, /T\.TabBar \{/);
  assert.match(bar, /contentItem: ListView \{/);
  assert.match(bar, /property alias currentIndex: bar\.currentIndex/);
  assert.match(bar, /default property alias tabs: bar\.contentData/);
  const btn = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/TabButton.qml", import.meta.url)), "utf8");
  assert.match(btn, /T\.TabButton \{/);
  assert.match(btn, /cssClass: \["tab"\]/);
  assert.match(btn, /cssClass: \["tab-label"\]/);
  assert.match(btn, /cssState: \(ctl\.checked \? \["selected"\] : \[\]\)\.concat\(ctl\.hovered \? \["hover"\] : \[\]\)/);
});

test("containers: <TabBar> rejects non-TabButton element children", async () => {
  await assert.rejects(
    qml(`export function F(){ return <TabBar><div /></TabBar>; }`),
    /children must be <TabButton>/,
  );
});

test("containers: <TabButton> outside <TabBar> throws", async () => {
  await assert.rejects(
    qml(`export function F(){ return <TabButton>Nope</TabButton>; }`),
    /only valid as a direct child of <TabBar>/,
  );
});

// ---------------------------------------------------------------------------
// <SplitView>
// ---------------------------------------------------------------------------

test("containers: <SplitView> defaults to horizontal orientation", async () => {
  const out = await qml(`export function F(){ return <SplitView class="sp"><div class="a" /><div class="b" /></SplitView>; }`);
  assert.match(out, /cssPrimitive: "splitview"/);
  assert.match(out, /T\.SplitView \{/);
  assert.match(out, /orientation: Qt\.Horizontal/);
});

test("containers: <SplitView orientation='vertical'> maps to Qt.Vertical", async () => {
  const out = await qml(`export function F(){ return <SplitView orientation="vertical"><div /><div /></SplitView>; }`);
  assert.match(out, /orientation: Qt\.Vertical/);
});

test("containers: <SplitView> handle is the stock SplitHandle, panes get fillWidth hint", async () => {
  const out = await qml(`export function F(){ return <SplitView><div /><div /></SplitView>; }`);
  // Handle is the stock component (plain-Rectangle root survives the CSS content-measure so it
  // keeps a real implicit thickness — a bare Css.CssRect measures 0 and the handle vanishes).
  assert.match(out, /handle: SplitHandle \{/);
  assert.match(out, /cssAncestor: __split0W/);
  assert.match(out, /horizontal: __split0\.orientation === Qt\.Horizontal/);
  // Panes carry a SplitView size hint so they share space (else the first pane's content eats it).
  assert.match(out, /T\.SplitView\.fillWidth: true/);
});

test("containers: SplitHandle stock component ships with the app", async () => {
  const app = await generate(`export function F(){ return <SplitView><div /><div /></SplitView>; }`, "/x.tsx");
  assert.ok(app.components.SplitHandle, "SplitHandle.qml should be in the components map");
  assert.match(app.components.SplitHandle, /T\.SplitHandle\.pressed/);
  assert.match(app.components.SplitHandle, /cssClass: \["handle"\]/);
});

test("containers: <SplitView> children emit as direct SplitView children", async () => {
  const out = await qml(`export function F(){ return <SplitView><div class="left" /><div class="right" /></SplitView>; }`);
  assert.match(out, /cssClass: \["left"\]/);
  assert.match(out, /cssClass: \["right"\]/);
});

// ---------------------------------------------------------------------------
// <Drawer>
// ---------------------------------------------------------------------------

const DRAWER = `
  export function F() {
    const [open, setOpen] = createSignal(false);
    return (
      <Drawer class="nav" open={open()} edge="left" onClose={() => setOpen(false)}>
        <text>Menu</text>
      </Drawer>
    );
  }
`;

test("containers: <Drawer> emits an in-tree anchor + T.Drawer on the overlay", async () => {
  const out = await qml(DRAWER);
  assert.match(out, /Css\.CssItem \{/);
  assert.match(out, /id: __drawer0W/);
  assert.match(out, /cssPrimitive: "drawer"/);
  assert.match(out, /cssClass: \["nav"\]/);
  assert.match(out, /T\.Drawer \{/);
  assert.match(out, /parent: T\.Overlay\.overlay/);
  assert.match(out, /edge: Qt\.LeftEdge/);
  assert.match(out, /dragMargin: 0/);
});

test("containers: <Drawer> background AND contentItem carry the cssAncestor re-anchor", async () => {
  const out = await qml(DRAWER);
  const matches = out.match(/property Item cssAncestor: __drawer0W/g);
  assert.equal(matches?.length, 2, "cssAncestor must appear on both background and contentItem");
  assert.match(out, /background: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["panel"\]/);
  assert.match(out, /contentItem: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["content"\]/);
});

test("containers: <Drawer open> binds visible via a RestoreNone Binding", async () => {
  const out = await qml(DRAWER);
  assert.match(out, /property: "visible"/);
  assert.match(out, /value: !!\(open\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("containers: <Drawer onClose> maps to onClosed", async () => {
  const out = await qml(DRAWER);
  assert.match(out, /onClosed: \{ open = false \}/);
});

test("containers: left drawer sizes width by the default 0.34 fraction, full height", async () => {
  const out = await qml(DRAWER);
  assert.match(out, /width: parent \? parent\.width \* \(0\.34\) : 0/);
  assert.match(out, /height: parent \? parent\.height : 0/);
});

test("containers: <Drawer edge='bottom' size={0.5}> transposes the axes", async () => {
  const out = await qml(`
    export function F() {
      const [open, setOpen] = createSignal(false);
      return <Drawer open={open()} edge="bottom" size={0.5}><text>Hi</text></Drawer>;
    }
  `);
  assert.match(out, /edge: Qt\.BottomEdge/);
  assert.match(out, /width: parent \? parent\.width : 0/);
  assert.match(out, /height: parent \? parent\.height \* \(0\.5\) : 0/);
});

test("containers: <Drawer> inside <Show> folds the guard into the visible Binding", async () => {
  const out = await qml(`
    export function F() {
      const [shown, setShown] = createSignal(true);
      const [open, setOpen] = createSignal(false);
      return <Show when={shown()}><Drawer open={open()}><text>Hi</text></Drawer></Show>;
    }
  `);
  assert.match(out, /value: !!\(shown\) && !!\(open\)/);
});

test("containers: <Drawer> rejects an unknown edge", async () => {
  await assert.rejects(
    qml(`export function F(){ return <Drawer edge="middle" />; }`),
    /edge must be left\|right\|top\|bottom/,
  );
});

// ---------------------------------------------------------------------------
// <StackView> — phase-1 simplified contract
// ---------------------------------------------------------------------------

test("containers: <StackView current> shows only child[current] (phase-1 contract)", async () => {
  const out = await qml(`
    export function F() {
      const [page, setPage] = createSignal(0);
      return (
        <StackView class="st" current={page()}>
          <div class="p0" />
          <div class="p1" />
        </StackView>
      );
    }
  `);
  assert.match(out, /W\.StackView \{/);
  assert.match(out, /cssClass: \["st"\]/);
  assert.match(out, /visible: !!\(\(page\) === 0\)/);
  assert.match(out, /visible: !!\(\(page\) === 1\)/);
  // The "stack" Css host now lives in StackView.qml.
  assert.doesNotMatch(out, /cssPrimitive: "stack"/);
});

test("containers: <StackView> without current defaults to page 0", async () => {
  const out = await qml(`export function F(){ return <StackView><div /><div /></StackView>; }`);
  assert.match(out, /visible: !!\(\(0\) === 0\)/);
  assert.match(out, /visible: !!\(\(0\) === 1\)/);
});

test("containers: StackView.qml is the pure Css 'stack' host", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/StackView.qml", import.meta.url)), "utf8");
  assert.match(src, /Css\.CssRect \{/);
  assert.match(src, /cssPrimitive: "stack"/);
});

// ---------------------------------------------------------------------------
// <SwipeView> + <PageIndicator>
// ---------------------------------------------------------------------------

const SWIPE = `
  export function F() {
    const [page, setPage] = createSignal(0);
    return (
      <div>
        <SwipeView class="sw" current={page()} onChange={(i) => setPage(i)}>
          <div class="pg-a" />
          <div class="pg-b" />
        </SwipeView>
        <PageIndicator class="dots" count={2} current={page()} />
      </div>
    );
  }
`;

test("containers: <SwipeView> emits T.SwipeView with a Basic-style ListView contentItem", async () => {
  const out = await qml(SWIPE);
  assert.match(out, /cssPrimitive: "swipeview"/);
  assert.match(out, /T\.SwipeView \{/);
  assert.match(out, /id: __swipe0/);
  assert.match(out, /contentItem: ListView \{/);
  assert.match(out, /model: __swipe0\.contentModel/);
  assert.match(out, /snapMode: ListView\.SnapOneItem/);
  assert.match(out, /highlightRangeMode: ListView\.StrictlyEnforceRange/);
  assert.match(out, /clip: true/);
});

test("containers: <SwipeView current> binds currentIndex via RestoreNone Binding", async () => {
  const out = await qml(SWIPE);
  assert.match(out, /target: __swipe0/);
  assert.match(out, /property: "currentIndex"/);
  assert.match(out, /value: page/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("containers: <SwipeView onChange> fires with the control's currentIndex", async () => {
  const out = await qml(SWIPE);
  assert.match(out, /onCurrentIndexChanged: \{ page = __swipe0\.currentIndex \}/);
});

test("containers: <SwipeView> pages emit as direct children", async () => {
  const out = await qml(SWIPE);
  assert.match(out, /cssClass: \["pg-a"\]/);
  assert.match(out, /cssClass: \["pg-b"\]/);
});

test("containers: <PageIndicator> wires count/currentIndex and emits the dot delegate", async () => {
  const out = await qml(SWIPE);
  assert.match(out, /cssPrimitive: "pageindicator"/);
  assert.match(out, /T\.PageIndicator \{/);
  assert.match(out, /count: 2/);
  assert.match(out, /currentIndex: page/);
  assert.match(out, /delegate: Css\.CssRect \{/);
  assert.match(out, /cssClass: \["dot"\]/);
  assert.match(out, /cssState: index === __dots1\.currentIndex \? \["selected"\] : \[\]/);
  assert.match(out, /implicitWidth: 8/);
  assert.match(out, /implicitHeight: 8/);
  // Templates create NO contentItem: the Row + Repeater must be emitted.
  assert.match(out, /contentItem: Row \{/);
  assert.match(out, /Repeater \{/);
  assert.match(out, /model: __dots1\.count/);
});

// ---------------------------------------------------------------------------
// Templates import
// ---------------------------------------------------------------------------

test("containers: using a migrated container widget prepends the solidqml.Widgets import", async () => {
  const out = await qmlType(`export function F(){ return <ToolBar><button>A</button></ToolBar>; }`);
  assert.match(out, /import solidqml\.Widgets .* as W/);
  // The T.ToolBar internals live in ToolBar.qml, so the emit no longer needs the Templates import.
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});

test("containers: <StackView> alone does NOT pull the Templates import (pure Css host)", async () => {
  const out = await qmlType(`export function F(){ return <StackView><div /></StackView>; }`);
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
});
