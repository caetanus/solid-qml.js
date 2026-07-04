/** Native menus group tests: <Menu>/<MenuItem>/<MenuSeparator>, <MenuBar>, <TreeView>, <Tray>.
 *
 *  Pattern mirrors test/widgets.test.ts: a local `qml()` helper for direct emitQml calls,
 *  and `qmlType()` for full emitComponentType output (import-line checks). */

import { test } from "node:test";
import assert from "node:assert/strict";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { emitComponentType } from "../src/emit/component.ts";
import type { Scope } from "../src/emit/expr.ts";
import * as t from "@babel/types";

/** Emit a render tree to QML (no component wrapper, direct emitQml). */
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

/** Emit a full component type (includes the prepended imports when widgets are used). */
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

const MENU_SRC = `
  export function F() {
    const [open, setOpen] = createSignal(false);
    const [last, setLast] = createSignal("");
    return (
      <div class="demo">
        <Menu open={open()} x={12} y={40} onClose={() => setOpen(false)}>
          <MenuItem onClick={() => setLast("new")}>New</MenuItem>
          <MenuItem onClick={() => setLast("open")}>Open</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => setLast("quit")}>Quit</MenuItem>
        </Menu>
      </div>
    );
  }
`;

// ---------------------------------------------------------------------------
// <Menu> — structure, controlled open, popup canon
// ---------------------------------------------------------------------------

test("menus: <Menu> emits T.Menu inside a zero-size Item host", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Item \{/);
  assert.match(out, /id: __menuHost0/);
  assert.match(out, /T\.Menu \{/);
  assert.match(out, /id: __menu0/);
});

test("menus: <Menu x/y> map to popup x/y", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /^\s*x: 12$/m);
  assert.match(out, /^\s*y: 40$/m);
});

test("menus: open={sig()} becomes a visible Binding with RestoreNone (emitInput idiom)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Binding \{/);
  assert.match(out, /target: __menu0/);
  assert.match(out, /property: "visible"/);
  assert.match(out, /value: !!\(open\)/);
  assert.match(out, /restoreMode: Binding\.RestoreNone/);
});

test("menus: onClose wires onClosed (setter lowered to signal assignment)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /onClosed: \{ open = false \}/);
});

test("menus: popup implicit sizes are set (Templates popups open 0x0 without them)", async () => {
  const out = await qml(MENU_SRC);
  // Width floor on the POPUP itself — a background CssFill's implicitWidth is clobbered by
  // the CSS engine's content pass, so implicitBackgroundWidth reads 0 under our engine.
  assert.match(out, /implicitWidth: Math\.max\(180, implicitContentWidth \+ leftPadding \+ rightPadding\)/);
  assert.match(out, /implicitHeight: Math\.max\(implicitBackgroundHeight \+ topInset \+ bottomInset, implicitContentHeight \+ topPadding \+ bottomPadding\)/);
});

test("menus: popupType is a native window (T.Popup.Window)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /popupType: T\.Popup\.Window/);
});

test("menus: cssAncestor re-anchor on BOTH background and contentItem", async () => {
  const out = await qml(MENU_SRC);
  const anchors = out.match(/property Item cssAncestor: __menuHost0/g) ?? [];
  assert.equal(anchors.length, 2, "background AND contentItem must re-anchor the CSS walk");
});

test("menus: background is a .popup CssFill and contentItem a ListView over contentModel", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /background: Css\.CssFill \{/);
  assert.match(out, /cssClass: \["demo"\]/); // wrapper div class
  assert.match(out, /cssClass: \["popup"\]/);
  assert.match(out, /contentItem: ListView \{/);
  assert.match(out, /model: __menu0\.contentModel/);
});

test("menus: author class on <Menu> merges into the popup background cssClass", async () => {
  const out = await qml(`export function F(){ return <Menu class="ctx" open={false}><MenuItem>A</MenuItem></Menu>; }`);
  assert.match(out, /cssClass: \["ctx", "popup"\]/);
});

test("menus: window deactivation closes the native popup (Qt::Popup semantics)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Window\.onActiveChanged: if \(!Window\.active\) __menu0\.close\(\)/);
});

// ---------------------------------------------------------------------------
// <MenuItem> / <MenuSeparator> inside <Menu>
// ---------------------------------------------------------------------------

test("menus: each <MenuItem> emits T.MenuItem with onTriggered from onClick", async () => {
  const out = await qml(MENU_SRC);
  const items = out.match(/T\.MenuItem \{/g) ?? [];
  assert.equal(items.length, 3);
  assert.match(out, /onTriggered: \{ last = "new" \}/);
  assert.match(out, /onTriggered: \{ last = "quit" \}/);
});

test("menus: MenuItem slots — .option background with highlighted→hover, .option-label text", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /cssClass: \["option"\]/);
  assert.match(out, /cssState: __mitem1\.highlighted \? \["hover"\] : \[\]/);
  assert.match(out, /cssClass: \["option-label"\]/);
  assert.match(out, /text: "New"/);
  assert.match(out, /text: __mitem1\.text/);
});

test("menus: <MenuSeparator> emits T.MenuSeparator with a 1px .sep CssRect", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /T\.MenuSeparator \{/);
  assert.match(out, /contentItem: Css\.CssRect \{/);
  assert.match(out, /cssClass: \["sep"\]/);
  assert.match(out, /implicitHeight: 1/);
});

test("menus: a non-item child of <Menu> throws a clear error", async () => {
  await assert.rejects(
    qml(`export function F(){ return <Menu open={false}><div /></Menu>; }`),
    /only accepts <MenuItem>\/<MenuSeparator>/,
  );
});

test("menus: a stray top-level <MenuItem> throws", async () => {
  await assert.rejects(
    qml(`export function F(){ return <MenuItem>Nope</MenuItem>; }`),
    /must be a child of <Menu>/,
  );
});

test("menus: qmlType pins QtQuick.Templates 6.8 (native-window popup)", async () => {
  const out = await qmlType(MENU_SRC);
  assert.match(out, /import QtQuick\.Templates 6\.8 as T/);
});

// ---------------------------------------------------------------------------
// <MenuBar>
// ---------------------------------------------------------------------------

const MENUBAR_SRC = `
  export function F() {
    const [last, setLast] = createSignal("");
    return (
      <MenuBar class="bar">
        <Menu title="File">
          <MenuItem onClick={() => setLast("new")}>New</MenuItem>
          <MenuSeparator />
          <MenuItem onClick={() => setLast("quit")}>Quit</MenuItem>
        </Menu>
        <Menu title="Edit">
          <MenuItem onClick={() => setLast("copy")}>Copy</MenuItem>
        </Menu>
      </MenuBar>
    );
  }
`;

test("menubar: CssFill wrapper mirrors T.MenuBar implicit sizes and carries the class", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssClass: \["bar"\]/);
  assert.match(out, /T\.MenuBar \{/);
  assert.match(out, /implicitWidth: __mbar0\.implicitWidth/);
  assert.match(out, /implicitHeight: __mbar0\.implicitHeight/);
});

test("menubar: contentItem is a Row Repeater over contentModel (Basic-style)", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /contentItem: Row \{/);
  assert.match(out, /Repeater \{ model: __mbar0\.contentModel \}/);
});

test("menubar: each <Menu title> becomes a T.MenuBarItem with an attached submenu", async () => {
  const out = await qml(MENUBAR_SRC);
  const items = out.match(/T\.MenuBarItem \{/g) ?? [];
  assert.equal(items.length, 2);
  assert.match(out, /menu: T\.Menu \{/);
  assert.match(out, /title: "File"/);
  assert.match(out, /title: "Edit"/);
});

test("menubar: item slots — .menubar-item background with hover/open, .menubar-label title text", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /cssClass: \["menubar-item"\]/);
  assert.match(out, /cssState: \(__mbi1\.hovered \? \["hover"\] : \[\]\)\.concat\(__mbi1\.highlighted \? \["open"\] : \[\]\)/);
  assert.match(out, /cssClass: \["menubar-label"\]/);
  assert.match(out, /text: __mbi1\.menu \? __mbi1\.menu\.title : ""/);
});

test("menubar: submenu popups re-anchor CSS at their MenuBarItem", async () => {
  const out = await qml(MENUBAR_SRC);
  // Counter walk: bar=0, File item=1, File menu=2, its 2 items=3/4, Edit item=5, Edit menu=6.
  assert.match(out, /property Item cssAncestor: __mbi1/);
  assert.match(out, /property Item cssAncestor: __mbi5/);
});

test("menubar: submenu items wire onTriggered", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /onTriggered: \{ last = "quit" \}/);
  assert.match(out, /onTriggered: \{ last = "copy" \}/);
});

test("menubar: a non-<Menu> child throws", async () => {
  await assert.rejects(
    qml(`export function F(){ return <MenuBar><div /></MenuBar>; }`),
    /<MenuBar> only accepts <Menu title/,
  );
});

// ---------------------------------------------------------------------------
// <TreeView>
// ---------------------------------------------------------------------------

const TREE_SRC = `
  export function F() {
    const [sel, setSel] = createSignal("");
    const data = [{ label: "src", children: [{ label: "emit" }] }, { label: "docs" }];
    return <TreeView class="files" data={data} onSelect={(node) => setSel(node.label)} />;
  }
`;

test("treeview: wrapper CssFill carries the class and sizes to the root column", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /Css\.CssFill \{/);
  assert.match(out, /cssClass: \["files"\]/);
  assert.match(out, /id: __tree0/);
  assert.match(out, /implicitHeight: __treeCol0\.height/);
});

test("treeview: recursion is a Component referenced BY ID (inline components cycle)", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /Component \{/);
  assert.match(out, /id: __treeComp0/);
  const delegateRefs = out.match(/delegate: __treeComp0/g) ?? [];
  assert.equal(delegateRefs.length, 2, "child Repeater AND root Repeater reference the node component");
});

test("treeview: rows are .tree-row CssFills with hover state and depth-indented label", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /cssClass: \["tree-row"\]/);
  assert.match(out, /cssState: __trowMa0\.containsMouse \? \["hover"\] : \[\]/);
  assert.match(out, /cssClass: \["tree-label"\]/);
  assert.match(out, /x: 8 \+ __tnode0\.depth \* 16 \+ 18/);
});

test("treeview: disclosure glyph flips with expanded and hides on leaves", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /text: \(__tnode0\.node && __tnode0\.node\.children && __tnode0\.node\.children\.length\) \? \(__tnode0\.expanded \? "▾" : "▸"\) : ""/);
  assert.match(out, /cssClass: \["tree-disclosure"\]/);
});

test("treeview: click toggles expanded and fires onSelect with the node", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /function __treeSel0\(__node\) \{ sel = __node\.label \}/);
  assert.match(out, /onClicked: \{ __tnode0\.expanded = !__tnode0\.expanded; __tree0\.__treeSel0\(__tnode0\.node\) \}/);
});

test("treeview: data expression feeds the root Repeater with depth-0 rows", async () => {
  const out = await qml(TREE_SRC);
  // `data` is a QML reserved name — safeName escapes the local to `data_`.
  assert.match(out, /model: \(\(data_\) \|\| \[\]\)\.map\(function\(c\) \{ return \(\{ __n: c, __d: 0 \}\) \}\)/);
});

test("treeview: children recurse with depth+1 only while expanded", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /model: \(__tnode0\.expanded && __tnode0\.node && __tnode0\.node\.children\) \? __tnode0\.node\.children\.map\(function\(c\) \{ return \(\{ __n: c, __d: __tnode0\.depth \+ 1 \}\) \}\) : \[\]/);
});

// ---------------------------------------------------------------------------
// <Tray>
// ---------------------------------------------------------------------------

const TRAY_SRC = `
  export function F() {
    const [n, setN] = createSignal(0);
    return (
      <Tray tooltip="solid-qml" onActivate={() => setN(n() + 1)}>
        <MenuItem onClick={() => setN(0)}>Reset</MenuItem>
      </Tray>
    );
  }
`;

test("tray: SystemTrayIcon wrapped in a zero-size Item (it is NOT an Item)", async () => {
  const out = await qml(TRAY_SRC);
  assert.match(out, /Item \{\n\s*width: 0\n\s*height: 0/);
  assert.match(out, /Platform\.SystemTrayIcon \{/);
  assert.match(out, /visible: true/);
});

test("tray: tooltip and onActivate wire through", async () => {
  const out = await qml(TRAY_SRC);
  assert.match(out, /tooltip: "solid-qml"/);
  assert.match(out, /onActivated: \{ n = n \+ 1 \}/);
});

test("tray: <MenuItem> children become a Platform.Menu with Platform.MenuItems", async () => {
  const out = await qml(TRAY_SRC);
  assert.match(out, /menu: Platform\.Menu \{/);
  assert.match(out, /Platform\.MenuItem \{/);
  assert.match(out, /text: "Reset"/);
  assert.match(out, /onTriggered: \{ n = 0 \}/);
});

test("tray: icon prop maps to icon.source", async () => {
  const out = await qml(`export function F(){ return <Tray icon="assets/tray.png" />; }`);
  assert.match(out, /icon\.source: "assets\/tray\.png"/);
});

test("tray: qmlType output carries the Qt.labs.platform import", async () => {
  const out = await qmlType(TRAY_SRC);
  assert.match(out, /import Qt\.labs\.platform 1\.1 as Platform/);
});
