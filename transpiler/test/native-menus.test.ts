/** Native menus group tests: <Menu>/<MenuItem>/<MenuSeparator>, <MenuBar>, <TreeView>, <Tray>.
 *
 *  Pattern mirrors test/widgets.test.ts: a local `qml()` helper for direct emitQml calls,
 *  and `qmlType()` for full emitComponentType output (import-line checks). */

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

test("menus: <Menu> instantiates W.Menu inside a zero-size Item host", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Item \{/);
  assert.match(out, /id: __menuHost0/);
  assert.match(out, /W\.Menu \{/);
  assert.match(out, /id: __menu0/);
  // The T.Menu shell now lives in Menu.qml, not the emit.
  assert.doesNotMatch(out, /T\.Menu \{/);
  assert.doesNotMatch(out, /popupType/);
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

test("menus: onClose wires the component's menuClosed signal", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /onMenuClosed: \{ open = false \}/);
});

test("menus: <Menu> re-anchors the CSS walk at the host via cssAncestor", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /cssAncestor: __menuHost0/);
});

test("menus: author class on <Menu> passes through as authorClass (component merges 'popup')", async () => {
  const out = await qml(`export function F(){ return <Menu class="ctx" open={false}><MenuItem>A</MenuItem></Menu>; }`);
  assert.match(out, /authorClass: \["ctx"\]/);
});

test("menus: window deactivation closes the native popup (Qt::Popup semantics)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Window\.onActiveChanged: if \(!Window\.active\) __menu0\.close\(\)/);
});

test("menus: Menu.qml hosts the T.Menu popup shell (implicit size, Popup.Item, cssAncestor slots)", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Menu.qml", import.meta.url)), "utf8");
  assert.match(src, /T\.Menu \{/);
  assert.match(src, /popupType: T\.Popup\.Item/);
  // Width floor on the POPUP itself — a background CssFill's implicitWidth is clobbered by the engine.
  assert.match(src, /implicitWidth: Math\.max\(180, implicitContentWidth \+ leftPadding \+ rightPadding\)/);
  assert.match(src, /implicitHeight: Math\.max\(implicitBackgroundHeight \+ topInset \+ bottomInset, implicitContentHeight \+ topPadding \+ bottomPadding\)/);
  assert.match(src, /padding: 1/);
  // background .popup CssFill (author class merged) + contentItem ListView over contentModel; BOTH
  // re-anchor the CSS walk (sibling slots covering every descendant).
  assert.match(src, /cssClass: ctl\.authorClass\.concat\(\["popup"\]\)/);
  assert.match(src, /model: ctl\.contentModel/);
  assert.equal((src.match(/property Item cssAncestor: ctl\.cssAncestor/g) ?? []).length, 2);
});

// ---------------------------------------------------------------------------
// <MenuItem> / <MenuSeparator> inside <Menu>
// ---------------------------------------------------------------------------

test("menus: each <MenuItem> emits W.MenuItem with onTriggered from onClick", async () => {
  const out = await qml(MENU_SRC);
  const items = out.match(/W\.MenuItem \{/g) ?? [];
  assert.equal(items.length, 3);
  assert.match(out, /text: "New"/);
  assert.match(out, /onTriggered: \{ last = "new" \}/);
  assert.match(out, /onTriggered: \{ last = "quit" \}/);
  // The .option/.option-label slots + cursor now live in MenuItem.qml, not the emit.
  assert.doesNotMatch(out, /T\.MenuItem/);
  assert.doesNotMatch(out, /option-label/);
});

test("menus: MenuItem.qml hosts the .option slots, cursor and mnemonic strip", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/MenuItem.qml", import.meta.url)), "utf8");
  assert.match(src, /T\.MenuItem \{/);
  assert.match(src, /cssClass: \["option"\]/);
  assert.match(src, /cssState: ctl\.highlighted \? \["hover"\] : \[\]/);
  assert.match(src, /cssClass: \["option-label"\]/);
  // Label strips the mnemonic marker (`&N` → N) for display.
  assert.match(src, /text: ctl\.text\.replace\(\/&\(\.\)\/g, "\$1"\)/);
  // Pointing-hand cursor (desktop affordance).
  assert.match(src, /HoverHandler \{ cursorShape: Qt\.PointingHandCursor \}/);
});

// ---------------------------------------------------------------------------
// <Menu trigger={…}> — self-managed, toggled by its embedded trigger
// ---------------------------------------------------------------------------

const TRIGGER_SRC = `
  export function F() {
    const [last, setLast] = createSignal("");
    return (
      <Menu trigger={<button>Actions</button>}>
        <MenuItem onClick={() => setLast("new")}>&New file</MenuItem>
        <MenuItem onClick={() => setLast("del")}>&Delete</MenuItem>
      </Menu>
    );
  }
`;

test("menus: <Menu trigger> toggles itself with a __closedAt debounce, no external Binding", async () => {
  const out = await qml(TRIGGER_SRC);
  // The toggle (reading the component's __closedAt) stays in the emit's trigger button.
  assert.match(out, /onClicked: \{ if \(__menu0\.visible\) __menu0\.close\(\); else if \(Date\.now\(\) - __menu0\.__closedAt > 250\) __menu0\.open\(\) \}/);
  // Self-managed: no controlled-open Binding element, and no author onClose signal.
  assert.doesNotMatch(out, /property: "visible"/);
  assert.doesNotMatch(out, /onMenuClosed/);
});

test("menus: Menu.qml records __closedAt on close (debounce store) alongside menuClosed", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Menu.qml", import.meta.url)), "utf8");
  assert.match(src, /property double __closedAt: 0/);
  assert.match(src, /onClosed: \{ __closedAt = Date\.now\(\); menuClosed\(\) \}/);
});

test("menus: <Menu trigger> emits the trigger and sizes the host to it", async () => {
  const out = await qml(TRIGGER_SRC);
  assert.match(out, /id: __mtrig0/);
  assert.match(out, /implicitWidth: __mtrig0\.implicitWidth/);
  assert.match(out, /y: __menuHost0\.height \+ 2/);
});

test("menus: <Menu trigger> that isn't a button throws a clear error", async () => {
  await assert.rejects(
    qml(`export function F(){ return <Menu trigger={<text>x</text>}><MenuItem>A</MenuItem></Menu>; }`),
    /trigger.*must be a <button>/,
  );
});

test("menus: <MenuSeparator> emits a bare W.MenuSeparator (internals in MenuSeparator.qml)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /W\.MenuSeparator \{ \}/);
  assert.doesNotMatch(out, /T\.MenuSeparator/);
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/MenuSeparator.qml", import.meta.url)), "utf8");
  assert.match(src, /T\.MenuSeparator \{/);
  assert.match(src, /contentItem: Css\.CssRect \{/);
  assert.match(src, /cssClass: \["sep"\]/);
  assert.match(src, /implicitHeight: 1/);
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

test("menus: qmlType imports solidqml.Widgets (the T.Menu shell now lives in Menu.qml)", async () => {
  const out = await qmlType(MENU_SRC);
  assert.match(out, /import solidqml\.Widgets 1\.0 as W/);
  // The generated component no longer emits T.* directly, so no Templates import is needed.
  assert.doesNotMatch(out, /import QtQuick\.Templates/);
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

test("menubar: each <Menu title> becomes a T.MenuBarItem with a W.Menu submenu", async () => {
  const out = await qml(MENUBAR_SRC);
  const items = out.match(/T\.MenuBarItem \{/g) ?? [];
  assert.equal(items.length, 2);
  assert.match(out, /menu: W\.Menu \{/);
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
  // Counter walk (items no longer consume the counter): bar=0, File item=1, File menu=2,
  // Edit item=3, Edit menu=4. Each submenu's W.Menu re-anchors at its MenuBarItem.
  assert.match(out, /cssAncestor: __mbi1/);
  assert.match(out, /cssAncestor: __mbi3/);
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

test("treeview: instantiates W.TreeView with the class and passes the data as treeData", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /W\.TreeView \{/);
  assert.match(out, /cssClass: \["files"\]/);
  // `data` is a QML reserved name (Item's default property) — safeName escaped the local to `data_`.
  assert.match(out, /__treeData: data_/);
  // The recursive-node internals now live in TreeView.qml, not the emit.
  assert.doesNotMatch(out, /Component \{/);
  assert.doesNotMatch(out, /tree-row/);
});

test("treeview: onSelect wires the component's selected(node) signal", async () => {
  const out = await qml(TREE_SRC);
  assert.match(out, /onSelected: \(node\) => \{ sel = node\.label \}/);
});

test("treeview: TreeView.qml hosts the recursive-node structure", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/TreeView.qml", import.meta.url)), "utf8");
  // Recursion is a Component referenced BY ID (a self-referencing inline component is a compile error).
  assert.match(src, /Component \{/);
  assert.match(src, /id: nodeComp/);
  assert.equal((src.match(/delegate: nodeComp/g) ?? []).length, 2, "child AND root Repeater reference the node component");
  // Rows: .tree-row with hover state, depth-indented .tree-label, flipping disclosure glyph.
  assert.match(src, /cssClass: \["tree-row"\]/);
  assert.match(src, /cssState: rowMa\.containsMouse \? \["hover"\] : \[\]/);
  assert.match(src, /x: 8 \+ nodeItem\.depth \* 16 \+ 18/);
  assert.match(src, /\? \(nodeItem\.expanded \? "▾" : "▸"\) : ""/);
  assert.match(src, /cssClass: \["tree-disclosure"\]/);
  // Click toggles expanded and fires selected(node); children recurse with depth+1 while expanded.
  assert.match(src, /onClicked: \{ nodeItem\.expanded = !nodeItem\.expanded; root\.selected\(nodeItem\.node\) \}/);
  assert.match(src, /model: \(\(root\.__treeData\) \|\| \[\]\)\.map\(function\(c\) \{ return \(\{ __n: c, __d: 0 \}\) \}\)/);
  assert.match(src, /model: \(nodeItem\.expanded && nodeItem\.node && nodeItem\.node\.children\) \? nodeItem\.node\.children\.map\(function\(c\) \{ return \(\{ __n: c, __d: nodeItem\.depth \+ 1 \}\) \}\) : \[\]/);
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

test("tray: instantiates W.Tray and wires tooltip + onActivate", async () => {
  const out = await qml(TRAY_SRC);
  assert.match(out, /W\.Tray \{/);
  assert.match(out, /tooltip: "solid-qml"/);
  assert.match(out, /onActivated: \{ n = n \+ 1 \}/);
  // No guard → the component's default `shown: true` drives the icon (no explicit shown line).
  assert.doesNotMatch(out, /Platform\.SystemTrayIcon/);
});

test("tray: <MenuItem> children build a Platform.Menu assigned through the menu alias", async () => {
  const out = await qml(TRAY_SRC);
  // The menu (with author item handlers) stays in the emit — registration is identical.
  assert.match(out, /menu: Platform\.Menu \{/);
  assert.match(out, /Platform\.MenuItem \{/);
  assert.match(out, /text: "Reset"/);
  assert.match(out, /onTriggered: \{ n = 0 \}/);
});

test("tray: no <MenuItem> children → no menu is set (registration unchanged)", async () => {
  const out = await qml(`export function F(){ return <Tray icon="assets/tray.png" />; }`);
  assert.doesNotMatch(out, /menu: Platform\.Menu/);
});

test("tray: icon prop maps to iconSource", async () => {
  const out = await qml(`export function F(){ return <Tray icon="assets/tray.png" />; }`);
  assert.match(out, /iconSource: "assets\/tray\.png"/);
});

test("tray: a Show guard folds into the component's shown", async () => {
  const out = await qml(`export function F(){ const [vis] = createSignal(true); return <Show when={vis()}><Tray tooltip="x" /></Show>; }`);
  assert.match(out, /shown: !!\(vis\(\)\)/);
});

test("tray: Tray.qml hosts the zero-size SystemTrayIcon shell", async () => {
  const src = await readFile(fileURLToPath(new URL("../../qml/solidqml/Widgets/Tray.qml", import.meta.url)), "utf8");
  assert.match(src, /width: 0/);
  assert.match(src, /height: 0/);
  assert.match(src, /Platform\.SystemTrayIcon \{/);
  assert.match(src, /visible: root\.shown/);
});

test("tray: qmlType output carries the Qt.labs.platform import (emit builds the Platform.Menu)", async () => {
  const out = await qmlType(TRAY_SRC);
  assert.match(out, /import Qt\.labs\.platform 1\.1 as Platform/);
});
