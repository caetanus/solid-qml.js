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
    let menuRef;
    const [last, setLast] = createSignal("");
    const openAt = (x, y) => menuRef.open(x, y);
    return (
      <div class="demo">
        <Menu ref={menuRef} onClose={() => setLast("closed")}>
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
// <Menu ref={r}> — imperative open, Popup.Window, anchor-move positioning
// ---------------------------------------------------------------------------

test("menus: <Menu ref> instantiates W.Menu inside a 1×1 anchor host", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /id: _ref_menuRef/);
  assert.match(out, /W\.Menu \{/);
  assert.match(out, /id: __menu0/);
  // The T.Menu shell now lives in Menu.qml, not the emit.
  assert.doesNotMatch(out, /T\.Menu \{/);
  assert.doesNotMatch(out, /popupType/);
});

test("menus: ref.open(x,y) moves the anchor host to the scene point then popup(0,0)", async () => {
  const out = await qml(MENU_SRC);
  // A Wayland Popup.Window anchors to parentItem's rect and ignores the popup(x,y) offset, so the
  // anchor host is MOVED to the click and opened with popup(0,0). Non-zero (1×1) keeps the rect valid.
  assert.match(out, /width: 1/);
  assert.match(out, /height: 1/);
  assert.match(out, /function open\(x, y\) \{ var __p = _ref_menuRef\.parent\.mapFromItem\(null, x, y\); _ref_menuRef\.x = __p\.x; _ref_menuRef\.y = __p\.y; __menu0\.popup\(0, 0\) \}/);
  // NO reactive visible Binding — the menu opens imperatively (fresh Wayland input serial).
  assert.doesNotMatch(out, /Binding \{/);
});

test("menus: <Menu open={sig}> is retired with a clear error pointing at ref+open", async () => {
  await assert.rejects(
    qml(`export function F(){ const [o] = createSignal(false); return <Menu open={o()}><MenuItem>A</MenuItem></Menu>; }`),
    /retired.*ref=\{r\}.*r\.open\(x, y\)/s,
  );
});

test("menus: onClose wires the component's menuClosed signal", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /onMenuClosed: \{ last = "closed" \}/);
});

test("menus: <Menu> re-anchors the CSS walk at the host via cssAncestor", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /cssAncestor: _ref_menuRef/);
});

test("menus: author class on <Menu> passes through as authorClass (component merges 'popup')", async () => {
  const out = await qml(`export function F(){ let r; return <Menu ref={r} class="ctx"><MenuItem>A</MenuItem></Menu>; }`);
  assert.match(out, /authorClass: \["ctx"\]/);
});

test("menus: window deactivation closes the native popup (Qt::Popup semantics)", async () => {
  const out = await qml(MENU_SRC);
  assert.match(out, /Window\.onActiveChanged: if \(!Window\.active\) __menu0\.close\(\)/);
});

test("menus: the C++ Menu hosts the popup shell (implicit size, Popup.Window, cssAncestor slots)", async () => {
  // Menu subclasses the PRIVATE QQuickMenu (it manages rows via casts to QQuickMenuItem); the
  // Css slot items ride as root-bound snippets.
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.h", import.meta.url)), "utf8");
  assert.match(src, /class Menu : public QQuickMenu/);
  // Popup.Window: its own popup surface so it overflows the app window (opened synchronously via the
  // ref/trigger/context forms so the Wayland grab gets a fresh input serial).
  assert.match(src, /setPopupType\(QQuickPopup::Window\)/);
  // Width floor on the POPUP itself — a background CssFill's implicitWidth is clobbered by the engine.
  assert.match(src, /setImplicitWidth\(qMax<qreal>\(180\.0, implicitContentWidth\(\) \+ leftPadding\(\) \+ rightPadding\(\)\)\)/);
  assert.match(src, /setPadding\(1\)/);
  // background .popup CssFill (author class merged) + contentItem ListView over contentModel; BOTH
  // re-anchor the CSS walk (sibling slots covering every descendant).
  assert.match(src, /cssClass: \(root\.authorClass \|\| \[\]\)\.concat\(\["popup"\]\)/);
  assert.match(src, /model: root\.contentModel/);
  assert.equal((src.match(/property Item cssAncestor: root\.cssAncestor/g) ?? []).length, 2);
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

test("menus: the C++ MenuItem hosts the .option slots, cursor and mnemonic underline", async () => {
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.h", import.meta.url)), "utf8");
  assert.match(src, /class MenuItem : public QQuickMenuItem/);
  assert.match(src, /cssClass: \["option"\]/);
  assert.match(src, /cssState: root\.highlighted \? \["hover"\] : \[\]/);
  assert.match(src, /cssClass: \["option-label"\]/);
  // The mnemonic marker renders as a real underline: `&N` → <u>N</u> via StyledText, with the
  // author text entity-escaped and `&&` as a literal ampersand.
  assert.match(src, /styledText: true/);
  assert.match(src, /text: root\.__mnemonicMarkup\(root\.text\)/);
  assert.match(src, /QLatin1String\("<u>"\) \+ s\.at\(i\) \+ QLatin1String\("<\/u>"\)/);
  // Pointing-hand cursor (desktop affordance).
  assert.match(src, /setCursor\(QCursor\(Qt::PointingHandCursor\)\)/);
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
  // The toggle opens SYNCHRONOUSLY in onClicked via popup() (fresh Wayland grab serial); the button
  // host is the anchor, so popup(0, host.height+2) opens the menu just below it. __closedAt debounce
  // swallows the dismiss click so a second click just closes.
  assert.match(out, /onClicked: \{ if \(__menu0\.visible\) __menu0\.close\(\); else if \(Date\.now\(\) - __menu0\.__closedAt > 250\) __menu0\.popup\(0, __menuHost0\.height \+ 2\) \}/);
  // Self-managed: no controlled-open Binding element, and no author onClose signal.
  assert.doesNotMatch(out, /property: "visible"/);
  assert.doesNotMatch(out, /onMenuClosed/);
});

test("menus: the C++ Menu records __closedAt on close (debounce store) alongside menuClosed", async () => {
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.h", import.meta.url)), "utf8");
  assert.match(src, /Q_PROPERTY\(double __closedAt READ closedAt NOTIFY closedAtChanged\)/);
  assert.match(src, /m_closedAt = static_cast<double>\(QDateTime::currentMSecsSinceEpoch\(\)\);[\s\S]*?emit menuClosed\(\);/);
});

test("menus: <Menu trigger> hosts the trigger in a Css box that joins the flex flow", async () => {
  const out = await qml(TRIGGER_SRC);
  assert.match(out, /id: __mtrig0/);
  // A plain Item host is INVISIBLE to the layout engine (only Css children flow), so a sibling
  // after the menu would overlap the trigger — the host is a class-less "div" measured from the
  // button by the engine's content pass.
  assert.match(out, /Css\.CssRect \{\n\s*id: __menuHost0\n\s*cssPrimitive: "div"/);
  assert.doesNotMatch(out, /implicitWidth: __mtrig0\.implicitWidth/);
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
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/menuwidgets.h", import.meta.url)), "utf8");
  assert.match(src, /class MenuSeparator : public QQuickMenuSeparator/);
  assert.match(src, /cssClass: \["sep"\]/);
  assert.match(src, /implicitHeight: 1/);
});

test("menus: a non-item child of <Menu> throws a clear error", async () => {
  await assert.rejects(
    qml(`export function F(){ let r; return <Menu ref={r}><div /></Menu>; }`),
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
      <MenuBar>
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

test("menubar: instantiates a native Qt.labs.platform MenuBar (OS window chrome, no CSS)", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /Platform\.MenuBar \{/);
  // OS chrome carries no scene CSS — the author class is dropped, not turned into a cssClass.
  assert.doesNotMatch(out, /cssClass: \["bar"\]/);
  assert.doesNotMatch(out, /W\.MenuBar/);
  // The emit registers the labs.platform import so the full component type carries it.
  assert.match(await qmlType(MENUBAR_SRC), /import Qt\.labs\.platform 1\.1 as Platform/);
});

test("menubar: each <Menu title> becomes a native Platform.Menu with its title", async () => {
  const out = await qml(MENUBAR_SRC);
  const menus = out.match(/Platform\.Menu \{/g) ?? [];
  assert.equal(menus.length, 2);
  assert.match(out, /title: "File"/);
  assert.match(out, /title: "Edit"/);
});

test("menubar: menu items become Platform.MenuItem with text; separators map to MenuSeparator", async () => {
  const out = await qml(MENUBAR_SRC);
  assert.match(out, /Platform\.MenuItem \{/);
  assert.match(out, /text: "New"/);
  assert.match(out, /text: "Quit"/);
  assert.match(out, /text: "Copy"/);
  assert.match(out, /Platform\.MenuSeparator \{ \}/);
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

test("treeview: TreeView.qml renders the flat visible-row list (root-held expansion state)", async () => {
  // TreeView is C++ now (widgets-to-cpp) — the snippet keeps the QML internals verbatim, with
  // the expansion state living on the snippet host (kb).
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/treeview.cpp", import.meta.url)), "utf8");
  // Expansion lives on the host (collapsed paths + rev counter), NOT in recursive delegates — the
  // flat `_visibleRows` order is what keyboard navigation walks. No Component-by-id recursion left.
  assert.match(src, /property var _collapsed: \(\{\}\)/);
  assert.match(src, /property int _rev: 0/);
  assert.match(src, /readonly property var _visibleRows/);
  assert.match(src, /model: kb\._visibleRows/);
  assert.doesNotMatch(src, /Component \{/);
  assert.doesNotMatch(src, /delegate: nodeComp/);
  // Rows: .tree-row with hover+selected states, depth-indented .tree-label, flipping disclosure glyph.
  assert.match(src, /cssClass: \["tree-row"\]/);
  assert.match(src, /cssState: \(rowMa\.containsMouse \? \["hover"\] : \[\]\)\.concat\(index === root\.currentIndex \? \["selected"\] : \[\]\)/);
  assert.match(src, /x: 8 \+ modelData\.__d \* 16 \+ 18/);
  assert.match(src, /modelData\.__k \? \(modelData\.__e \? "▾" : "▸"\) : ""/);
  assert.match(src, /cssClass: \["tree-disclosure"\]/);
  // Click focuses the tree, selects the row, toggles a branch — capturing the node BEFORE the
  // toggle rebuilds the delegate's modelData context.
  assert.match(src, /kb\.forceActiveFocus\(\);\n\s*\/\/[^\n]*\n\s*\/\/[^\n]*\n\s*var node = modelData\.__n;/);
  assert.match(src, /if \(modelData\.__k\) kb\._toggle\(modelData\.__p\);\n\s*root\.selected\(node\);/);
});

test("treeview keyboard: ONE tab stop; Up/Down move, Right expands/descends, Left collapses/ascends, Enter re-commits", async () => {
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/treeview.cpp", import.meta.url)), "utf8");
  assert.match(src, /activeFocusOnTab: true/);
  assert.match(src, /Keys\.onUpPressed: kb\._move\(-1\)/);
  assert.match(src, /Keys\.onDownPressed: kb\._move\(1\)/);
  assert.match(src, /Keys\.onRightPressed: kb\._expandOrDescend\(\)/);
  assert.match(src, /Keys\.onLeftPressed: kb\._collapseOrAscend\(\)/);
  // Enter mirrors a click (toggle + commit); Space only toggles.
  assert.match(src, /Keys\.onReturnPressed: \{ kb\._toggleCurrent\(\); kb\._emitCurrent\(\); \}/);
  assert.match(src, /Keys\.onEnterPressed: \{ kb\._toggleCurrent\(\); kb\._emitCurrent\(\); \}/);
  assert.match(src, /Keys\.onSpacePressed: kb\._toggleCurrent\(\)/);
  // Left on a leaf/collapsed row ascends to the parent path (path minus its last segment).
  assert.match(src, /r\.__p\.lastIndexOf\("\/"\)/);
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

test("tray: under Show the icon exists only while the guard holds", async () => {
  const out = await qml(`export function F(){ const [vis] = createSignal(true); return <Show when={vis()}><Tray tooltip="x" /></Show>; }`);
  // Getter-only createSignal destructure is a real signal: the guard is a reactive property READ.
  assert.match(out, /Repeater \{\s*model: \(vis\) \? 1 : 0\s*W\.Tray \{/);
});

test("tray: the C++ Tray hosts the zero-size SystemTrayIcon shell", async () => {
  // Tray is C++ now (widgets-to-cpp) — the snippet keeps the Platform icon verbatim.
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/tray.cpp", import.meta.url)), "utf8")
    + await readFile(fileURLToPath(new URL("../../src/widgets/tray.h", import.meta.url)), "utf8");
  assert.match(src, /setWidth\(0\)/);
  assert.match(src, /setHeight\(0\)/);
  assert.match(src, /Platform\.SystemTrayIcon \{/);
  assert.match(src, /visible: root\.shown/);
  assert.match(src, /menu: root\.menu/);
});

test("tray: qmlType output carries the Qt.labs.platform import (emit builds the Platform.Menu)", async () => {
  const out = await qmlType(TRAY_SRC);
  assert.match(out, /import Qt\.labs\.platform 1\.1 as Platform/);
});

test("shortcut: <Shortcut keys onActivated> → QML Shortcut with sequences", async () => {
  const single = await qml(`export function F(){ return <Shortcut keys="Ctrl+S" onActivated={() => 0} />; }`);
  assert.match(single, /Shortcut \{\n\s*sequences: \["Ctrl\+S"\]\n\s*onActivated: \{ 0 \}/);
  const multi = await qml(`export function F(){ return <Shortcut keys={["Ctrl+Q","Ctrl+W"]} onActivated={() => 0} />; }`);
  assert.match(multi, /sequences: \["Ctrl\+Q", "Ctrl\+W"\]/);
});

test("listview: <ListView data onSelect> → W.ListView with __listData + selected(item,index)", async () => {
  const out = await qml(`export function F(){ return <ListView class="lst" data={["a","b"]} onSelect={(it) => 0} />; }`);
  assert.match(out, /W\.ListView \{/);
  assert.match(out, /__listData: \["a", "b"\]/);
  assert.match(out, /onSelected: \(item, __idx\) => \{ 0 \}/);
  // ListView is C++ now (widgets-to-cpp) — the snippet keeps the QML internals verbatim.
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/listview.cpp", import.meta.url)), "utf8");
  assert.match(src, /import QtQuick as QtQ/);
  assert.match(src, /QtQ\.ListView \{/);
  assert.match(src, /cssClass: \["list-item"\]/);
});

test("tableview: <TableView columns data onSelect> → W.TableView with __columns/__rows + selected", async () => {
  const out = await qml(`export function F(){ return <TableView class="tv" columns={[{key:"n",label:"N"}]} data={[{n:1}]} onSelect={(r) => 0} />; }`);
  assert.match(out, /W\.TableView \{/);
  assert.match(out, /__columns: \[\(\{ key: "n", label: "N" \}\)\]/);
  assert.match(out, /__rows: \[\(\{ n: 1 \}\)\]/);
  assert.match(out, /onSelected: \(row, __idx\) => \{ 0 \}/);
  // TableView is C++ now (widgets-to-cpp) — the snippet keeps the QML internals verbatim.
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/tableview.cpp", import.meta.url)), "utf8");
  assert.match(src, /import QtQuick as QtQ/);
  assert.match(src, /cssClass: \["table-header"\]/);
  assert.match(src, /cssClass: \["table-cell"\]/);
});

test("tableview sort: header click toggles column sort (▲/▼, numeric + locale compare, squelched reset)", async () => {
  const src = await readFile(fileURLToPath(new URL("../../src/widgets/tableview.cpp", import.meta.url)), "utf8");
  assert.match(src, /property int sortColumn: -1/);
  assert.match(src, /property bool sortAscending: true/);
  assert.match(src, /onClicked: host\._toggleSort\(thCell\.colIndex\)/);
  assert.match(src, /sortAscending \? " {2}▲" : " {2}▼"/); // trailing marker on the sorted header
  assert.match(src, /\(av - bv\) \* dir/); // numbers compare numerically
  assert.match(src, /localeCompare\("" \+ bv\) \* dir/); // everything else as strings
  // The model swap forces currentIndex=0 eagerly — squelch must be armed BEFORE the sort state flips.
  assert.match(src, /lv\._squelch = true;\n\s*if \(host\.sortColumn === col\)/);
  assert.match(src, /Qt\.callLater\(function \(\) \{ lv\.currentIndex = -1; lv\._squelch = false; \}\)/);
});

test("listview/tableview keyboard: inner ListView is the tab stop, arrows navigate, Enter re-commits, no mount emit", async () => {
  for (const f of ["listview.cpp", "tableview.cpp"]) {
    const src = await readFile(fileURLToPath(new URL(`../../src/widgets/${f}`, import.meta.url)), "utf8");
    assert.match(src, /activeFocusOnTab: true/, f);
    assert.match(src, /keyNavigationEnabled: true/, f);
    // Keys is an attached type from the aliased QtQuick import — MUST be QtQ-qualified (bare Keys
    // is "Non-existent attached object" and kills the whole component).
    assert.match(src, /QtQ\.Keys\.onReturnPressed: host\._emitCurrent\(\)/, f);
    assert.match(src, /QtQ\.Keys\.onEnterPressed: host\._emitCurrent\(\)/, f);
    // QtQuick forces currentIndex=0 on model load; the squelch keeps mount from firing selected().
    assert.match(src, /property bool _squelch: true/, f);
    assert.match(src, /if \(_squelch\)\s*return;/, f);
    assert.match(src, /QtQ\.Component\.onCompleted: Qt\.callLater\(function \(\) \{ lv\.currentIndex = -1; lv\._squelch = false; \}\)/, f);
    // Selection follows the INNER view's currentIndex (a click focuses it first).
    assert.match(src, /lv\.forceActiveFocus\(\)/, f);
  }
});

test("contextmenu: <ContextMenu> fills its parent and opens at the cursor on right-click", async () => {
  const out = await qml(`export function F(){ const [last, setLast] = createSignal(""); return (
    <div class="zone">
      <ContextMenu onClose={() => setLast("closed")}>
        <MenuItem onClick={() => setLast("copy")}>&Copy</MenuItem>
        <MenuSeparator />
        <MenuItem onClick={() => setLast("del")}>&Delete</MenuItem>
      </ContextMenu>
    </div>); }`);
  // Foreign anchored host: invisible to the CSS layout pass, covers the containing box.
  assert.match(out, /Item \{\n\s*id: __menuHost0\n\s*anchors\.fill: parent/);
  // Right-button only — left clicks/hover/wheel pass through to the content below.
  assert.match(out, /acceptedButtons: Qt\.RightButton/);
  // A Wayland Popup.Window anchors to parentItem's rect, so the click MOVES a 1×1 anchor to the
  // cursor then opens with popup(0,0) (the offset is ignored on Wayland).
  assert.match(out, /onClicked: function\(mouse\) \{ __ctxAnchor0\.x = mouse\.x; __ctxAnchor0\.y = mouse\.y; __menu0\.popup\(0, 0\) \}/);
  assert.match(out, /id: __ctxAnchor0\n\s*width: 1\n\s*height: 1/);
  assert.match(out, /cssAncestor: __ctxAnchor0/);
  assert.match(out, /W\.Menu \{/);
  assert.match(out, /onMenuClosed: \{ last = "closed" \}/);
  assert.equal((out.match(/W\.MenuItem \{/g) ?? []).length, 2);
  assert.match(out, /W\.MenuSeparator \{ \}/);
});

test("contextmenu: suppressWhen declines the press instead of opening (solidterm mouse reporting)", async () => {
  const out = await qml(`export function F(){ let term; return (
    <div class="zone">
      <ContextMenu suppressWhen={(mods) => term.wantsMouseReportFocused(mods)}>
        <MenuItem onClick={() => {}}>&Copy</MenuItem>
      </ContextMenu>
    </div>); }`);
  // The guard runs on PRESS (not click): a true result declines the event so Qt Quick's hit-test
  // falls through to whatever is below this host, before any menu/popup bookkeeping happens.
  assert.match(out, /onPressed: function\(mouse\) \{ if \(term\.wantsMouseReportFocused\(mouse\.modifiers\)\) mouse\.accepted = false \}/);
  // onClicked (the popup-opening path) is unconditional and unchanged — it only ever runs when
  // onPressed did NOT decline.
  assert.match(out, /onClicked: function\(mouse\) \{ __ctxAnchor0\.x = mouse\.x; __ctxAnchor0\.y = mouse\.y; __menu0\.popup\(0, 0\) \}/);
});

test("contextmenu: a block-bodied suppressWhen runs its statements and uses its return value", async () => {
  const out = await qml(`export function F(){ let term; let seen = ""; return (
    <div class="zone">
      <ContextMenu suppressWhen={(mods) => { seen = "x"; return term.wantsMouseReportFocused(mods); }}>
        <MenuItem onClick={() => {}}>&Copy</MenuItem>
      </ContextMenu>
    </div>); }`);
  const line = out.split("\n").find((l) => l.includes("onPressed:")) ?? "";
  // Statements can't sit inside `if (…)`: the body becomes an immediately-invoked function.
  assert.match(line, /if \(\(function\(\) \{[^]*return term\.wantsMouseReportFocused\(mouse\.modifiers\);[^]*\}\)\(\)\) mouse\.accepted = false/);
  assert.doesNotMatch(line, /if \([^(]*=[^=][^;]*;/); // no statement inside the bare condition
  // The emitted handler parses as JavaScript.
  const body = line.replace(/^\s*onPressed:\s*/, "");
  assert.doesNotThrow(() => new Function(`return (${body});`));
});

test("contextmenu: without suppressWhen emits no onPressed guard (unchanged default behaviour)", async () => {
  const out = await qml(`export function F(){ return (
    <div class="zone">
      <ContextMenu>
        <MenuItem onClick={() => {}}>&Copy</MenuItem>
      </ContextMenu>
    </div>); }`);
  assert.doesNotMatch(out, /onPressed:/);
});

test("contextmenu: rejects non-menu children", async () => {
  await assert.rejects(
    qml(`export function F(){ return <div><ContextMenu><div /></ContextMenu></div>; }`),
    /only accepts <MenuItem>/,
  );
});

// ---------------------------------------------------------------------------
// <For>/<Show> inside <Menu>/<ContextMenu> — dynamic items (spell-check menu, solid-player)
// ---------------------------------------------------------------------------

const SPELL_MENU_SRC = `
  export function F() {
    let spellMenu;
    const [sugg, setSugg] = createSignal([]);
    const replaceWith = (w) => setSugg([]);
    const addToDictionary = () => setSugg([]);
    return (
      <Menu ref={spellMenu}>
        <For each={sugg()}>{(w) => <MenuItem onClick={() => replaceWith(w)}>{w}</MenuItem>}</For>
        <MenuSeparator />
        <MenuItem onClick={() => addToDictionary()}>Add to dictionary</MenuItem>
      </Menu>
    );
  }
`;

test("menus <For>: an Instantiator whose delegate IS the row, wired to insertItem/removeItem (Qt's own documented dynamic-menu-items pattern — no C++ change needed)", async () => {
  const out = await qml(SPELL_MENU_SRC);
  assert.match(out,
    /Instantiator \{\n\s*id: __menuDyn\d+\n\s*model: sugg\n\s*delegate: Component \{\n\s*W\.MenuItem \{\n\s*text: \(modelData\)\n\s*onTriggered: \{ replaceWith\(modelData\) \}\n\s*\}\n\s*\}\n\s*onObjectAdded: \(index, object\) => __menu0\.insertItem\(0 \+ index, object\)\n\s*onObjectRemoved: \(index, object\) => __menu0\.removeItem\(object\)\n\s*\}/);
  // The static items before/after the <For> are untouched, in source order.
  assert.match(out, /\}\n\s*W\.MenuSeparator \{ \}\n\s*W\.MenuItem \{\n\s*text: "Add to dictionary"/);
});

test("menus <For>: i() routes through the reserved __forIndex property, never the bare `index`", async () => {
  const out = await qml(`export function F(){
    let r; const [sugg] = createSignal(["a","b"]);
    return <Menu ref={r}><For each={sugg()}>{(w, i) => <MenuItem onClick={() => console.log(w, i())}>{w}</MenuItem>}</For></Menu>;
  }`);
  assert.match(out, /delegate: Component \{\n\s*W\.MenuItem \{\n\s*property int __forIndex: index\n\s*text: \(modelData\)\n\s*onTriggered: \{ console\.log\(modelData, __forIndex\) \}/);
  assert.doesNotMatch(out, /console\.log\(modelData, index\)/);
});

test("menus <For>: multiple <For> blocks — each an independent Instantiator; the SECOND's insertItem index accounts for the static separator AND the first block's live .count", async () => {
  const out = await qml(`export function F(){
    let r; const [a] = createSignal(["x"]); const [b] = createSignal(["y"]);
    return <Menu ref={r}>
      <For each={a()}>{(w) => <MenuItem>{w}</MenuItem>}</For>
      <MenuSeparator />
      <For each={b()}>{(w) => <MenuItem>{w}</MenuItem>}</For>
    </Menu>;
  }`);
  assert.equal((out.match(/Instantiator \{/g) ?? []).length, 2);
  const m1 = /id: (__menuDyn\d+)\n\s*model: a[\s\S]*?onObjectAdded: \(index, object\) => __menu0\.insertItem\((\d+) \+ index, object\)/.exec(out);
  assert.ok(m1, "first For block: no static before it, no .count term");
  const [, id1, base1] = m1!;
  assert.equal(base1, "0"); // 1st: no static before it
  const m2 = new RegExp(`id: (__menuDyn\\d+)\\n\\s*model: b[\\s\\S]*?onObjectAdded: \\(index, object\\) => __menu0\\.insertItem\\((\\d+) \\+ ${id1}\\.count \\+ index, object\\)`).exec(out);
  // 2nd: one static (the separator) + the first block's own running count.
  assert.ok(m2, "second For block: base includes the static separator + the first block's .count");
  assert.equal(m2![2], "1");
});

test("menus <For>: a <MenuSeparator> row (no text/onTriggered)", async () => {
  const out = await qml(`export function F(){
    let r; const [a] = createSignal([1,2]);
    return <Menu ref={r}><For each={a()}>{() => <MenuSeparator />}</For></Menu>;
  }`);
  assert.match(out, /Instantiator \{\n\s*id: __menuDyn\d+\n\s*model: a\n\s*delegate: Component \{\n\s*W\.MenuSeparator \{ \}\n\s*\}/);
});

test("menus <For>: row must be a single <MenuItem>/<MenuSeparator> — a <div> row fails loud", async () => {
  await assert.rejects(
    qml(`export function F(){ let r; return <Menu ref={r}><For each={[1]}>{(w) => <div>{w}</div>}</For></Menu>; }`),
    /<Menu><For> row must be a single <MenuItem>\/<MenuSeparator>, got <div>/,
  );
});

test("menus <Show>: wraps one or more <MenuItem>/<MenuSeparator> — one Instantiator per item, the second's index accounting for the first", async () => {
  const out = await qml(`export function F(){
    let r; const [adv, setAdv] = createSignal(true);
    return <Menu ref={r}>
      <Show when={adv()}>
        <MenuItem onClick={() => setAdv(false)}>Advanced</MenuItem>
        <MenuSeparator />
      </Show>
      <MenuItem>Always</MenuItem>
    </Menu>;
  }`);
  const m1 = /Instantiator \{\n\s*id: (__menuDyn\d+)\n\s*model: \(adv\) \? 1 : 0\n\s*delegate: Component \{\n\s*W\.MenuItem \{\n\s*text: "Advanced"\n\s*onTriggered: \{ adv = false \}\n\s*\}\n\s*\}\n\s*onObjectAdded: \(index, object\) => __menu0\.insertItem\((\d+) \+ index, object\)/.exec(out);
  assert.ok(m1, "first Show item (MenuItem): base has no .count term (nothing dynamic precedes it)");
  assert.equal(m1![2], "0");
  const m2 = new RegExp(`Instantiator \\{\\n\\s*id: __menuDyn\\d+\\n\\s*model: \\(adv\\) \\? 1 : 0\\n\\s*delegate: Component \\{\\n\\s*W\\.MenuSeparator \\{ \\}\\n\\s*\\}\\n\\s*onObjectAdded: \\(index, object\\) => __menu0\\.insertItem\\(0 \\+ ${m1![1]}\\.count \\+ index, object\\)`).exec(out);
  assert.ok(m2, "second Show item (separator): base folds in the first item's own Instantiator .count");
  assert.match(out, /W\.MenuItem \{\n\s*text: "Always"/);
});

test("menus <Show>: nesting <For>/<Show> inside the gate is out of scope — fails loud", async () => {
  await assert.rejects(
    qml(`export function F(){ let r; return <Menu ref={r}><Show when={true}><For each={[1]}>{(w) => <MenuItem>{w}</MenuItem>}</For></Show></Menu>; }`),
    /<Menu><Show> only wraps <MenuItem>\/<MenuSeparator>, got <For>/,
  );
  await assert.rejects(
    qml(`export function F(){ let r; return <Menu ref={r}><Show when={true} fallback={<MenuItem>x</MenuItem>}><MenuItem>y</MenuItem></Show></Menu>; }`),
    /<Show fallback> is not supported inside a menu/,
  );
});

test("menus: disabled/checked on a <MenuItem> (static) — new reactive props, zero cost when absent", async () => {
  const out = await qml(`export function F(){
    let r; const [wrap, setWrap] = createSignal(true);
    return <Menu ref={r}><MenuItem checked={wrap()} disabled={!wrap()}>Word wrap</MenuItem></Menu>;
  }`);
  assert.match(out, /W\.MenuItem \{\n\s*text: "Word wrap"\n\s*checkable: true\n\s*checked: wrap\n\s*enabled: !\(!wrap\)\n\s*\}/);
});

test("menus <For>: row items get disabled/checked too (reactive per row, via the row value)", async () => {
  const out = await qml(`export function F(){
    let r; const [items] = createSignal([{ label: "a", on: true }]);
    return <Menu ref={r}><For each={items()}>{(it) => <MenuItem checked={it.on} disabled={!it.on}>{it.label}</MenuItem>}</For></Menu>;
  }`);
  assert.match(out, /checkable: true\n\s*checked: modelData\.on\n\s*enabled: !\(!modelData\.on\)/);
});

test("menus: an unrecognized <MenuItem> attribute still fails loud (attrs.ts contract, not silently dropped)", async () => {
  await assert.rejects(
    qml(`export function F(){ let r; return <Menu ref={r}><MenuItem icon="x.png">Open</MenuItem></Menu>; }`),
    /<MenuItem> does not support `icon`/,
  );
});

test("contextmenu <For>: the same dynamic-item support (shares emitMenuItemLines with <Menu>)", async () => {
  const out = await qml(`export function F(){
    const [sugg] = createSignal(["teh", "the"]);
    return <div class="zone"><ContextMenu>
      <For each={sugg()}>{(w) => <MenuItem onClick={() => 0}>{w}</MenuItem>}</For>
      <MenuSeparator />
      <MenuItem onClick={() => 0}>Add to dictionary</MenuItem>
    </ContextMenu></div>;
  }`);
  assert.match(out, /Instantiator \{\n\s*id: __menuDyn\d+\n\s*model: sugg\n\s*delegate: Component \{\n\s*W\.MenuItem \{\n\s*text: \(modelData\)/);
  assert.match(out, /W\.MenuItem \{\n\s*text: "Add to dictionary"/);
});

test("menu trigger <For>: the self-managed trigger form shares the same dynamic-item path", async () => {
  const out = await qml(`export function F(){
    const [sugg] = createSignal(["a"]);
    return <Menu trigger={<button>Actions</button>}>
      <For each={sugg()}>{(w) => <MenuItem onClick={() => 0}>{w}</MenuItem>}</For>
    </Menu>;
  }`);
  assert.match(out, /Instantiator \{\n\s*id: __menuDyn\d+\n\s*model: sugg\n\s*delegate: Component \{\n\s*W\.MenuItem \{\n\s*text: \(modelData\)/);
});
