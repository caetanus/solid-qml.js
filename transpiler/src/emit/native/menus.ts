// Native menus group: <Menu>/<MenuItem>/<MenuSeparator>, <MenuBar>, <TreeView>, <Tray>.
//
// <Menu>/<MenuBar> are QtQuick TEMPLATES (T.Menu is a QQuickPopup; T.MenuBar a QQuickContainer —
// see ~/src/qtdeclarative/src/quicktemplates/qquickmenu_p.h / qquickmenubar_p.h). Their visual
// slots are filled with Css* items following the emitSelect popup canon in ../qml.ts:
//   - Templates popups have NO implicit-size policy of their own (that is the style's job, and
//     we ARE the style) — without explicit implicitWidth/Height a menu opens 0x0.
//   - popup contents reparent to the window Overlay, severing the visual chain CSS scoping walks;
//     `property Item cssAncestor: <owner>` on BOTH background and contentItem re-anchors the walk
//     (they are sibling slots — every popup descendant passes through one of them).
//   - popupType: T.Popup.Window (Qt 6.8) for real native dropdown windows, with the owner's
//     Qt::Popup semantics: close when the app window deactivates.
//
// <TreeView> is OUR OWN recursive tree over plain {label, children?} objects. Qt's TreeView
// requires a QAbstractItemModel and brings delegate recycling; virtualization is a LATER phase
// (plan: 2026-07-03-native-only-widgets, Phase 4) — this emitter targets config-panel-sized
// trees. Recursion mechanism: a `Component { id }` whose delegate references itself BY ID
// (runtime reference). QML inline components (`component X: …`) were probed and REJECTED here:
// a self-reference makes the compiler fail with "Inline components form a cycle!".
//
// <Tray> maps to Qt.labs.platform SystemTrayIcon — a QObject, NOT an Item — so it is wrapped
// in a zero-size Item to sit in the tree wherever declared.
import * as t from "@babel/types";
import { registerNativeTags, requireImport } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { buildCssClassLine, guardLine, INDENT } from "../qml.ts";
import { hParts, isHCall } from "../../ast/h.ts";
import { safeName } from "../../names/safe.ts";

type Fn = t.ArrowFunctionExpression | t.FunctionExpression;

// ─── small prop/handler helpers (registry emitters read raw h() props) ─────────────────────────

function propsOf(propsArg: t.Node | undefined): Map<string, t.Expression> {
  const map = new Map<string, t.Expression>();
  if (!propsArg || !t.isObjectExpression(propsArg)) return map;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
    map.set(p.key.name, p.value);
  }
  return map;
}

function classesOf(props: Map<string, t.Expression>): string[] {
  const c = props.get("class");
  return c && t.isStringLiteral(c) ? c.value.split(/\s+/).filter(Boolean) : [];
}

/** Props-shaped shim so wrapper emitters reuse buildCssClassLine (static class + classList). */
function cssPropsShim(propsArg: t.Node | undefined) {
  const props = propsOf(propsArg);
  const classList: Array<{ key: string; expr: t.Expression }> = [];
  const cl = props.get("classList");
  if (cl && t.isObjectExpression(cl)) {
    for (const cp of cl.properties) {
      if (!t.isObjectProperty(cp)) continue;
      const key = t.isIdentifier(cp.key) ? cp.key.name : t.isStringLiteral(cp.key) ? cp.key.value : null;
      if (key && t.isExpression(cp.value)) classList.push({ key, expr: cp.value });
    }
  }
  return {
    classes: classesOf(props), classList,
    onClick: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined,
  };
}

/** Author handler → QML statement body. Arrow/function expressions (block or expression bodied);
 *  the optional first parameter is renamed to `paramQml` in the emitted body (translateToggleHandler
 *  idiom). A bare helper identifier (`onClick={quit}`) becomes a call. */
function handlerBody(node: t.Node, scope: Scope, paramQml?: string): string {
  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) {
    const fn = node as Fn;
    const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
    const inner: Scope = {
      ...scope, mode: "handler",
      locals: { ...(scope.locals ?? {}), ...(paramName && paramQml ? { [paramName]: paramQml } : {}) },
    };
    if (t.isBlockStatement(fn.body)) {
      return fn.body.body.map((s) => {
        if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
        if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
        return "";
      }).filter(Boolean).join(" ");
    }
    return emitExpr(fn.body, inner);
  }
  if (t.isIdentifier(node) && scope.helpers?.has(node.name)) return `${safeName(node.name)}()`;
  throw new Error("native menus: only inline arrow/function handlers (or a helper reference) are supported");
}

/** String binding from text + interpolation children (same shape as qml.ts textBinding). */
function textBindingOf(children: t.Node[], scope: Scope): string {
  const parts: string[] = [];
  for (const child of children) {
    if (t.isStringLiteral(child)) {
      if (child.value.trim()) parts.push(JSON.stringify(child.value.trim()));
    } else if (child.type === "JSXText") {
      const cleaned = (child as unknown as { value: string }).value.replace(/\s+/g, " ").trim();
      if (cleaned) parts.push(JSON.stringify(cleaned));
    } else if (t.isExpression(child)) {
      parts.push(`(${emitExpr(child, { ...scope, mode: "binding" })})`);
    }
  }
  if (parts.length === 0) return `""`;
  if (parts.length === 1) return parts[0];
  return parts[0].startsWith('"') ? parts.join(" + ") : `"" + ${parts.join(" + ")}`;
}

// ─── <Menu> core (shared by the standalone tag and MenuBar submenus) ────────────────────────────

interface MenuChild { kind: "item" | "separator"; labelBinding?: string; onClick?: t.Node }

function parseMenuChildren(children: t.Node[], scope: Scope, owner: string): MenuChild[] {
  const out: MenuChild[] = [];
  for (const child of children) {
    if (!isHCall(child)) continue; // whitespace between items
    const { tag, props: itemProps, children: itemKids } = hParts(child as t.CallExpression);
    const name = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : "?";
    if (name === "MenuSeparator") { out.push({ kind: "separator" }); continue; }
    if (name !== "MenuItem") throw new Error(`<${owner}> only accepts <MenuItem>/<MenuSeparator> children, got <${name}>`);
    const p = propsOf(itemProps);
    out.push({ kind: "item", labelBinding: textBindingOf(itemKids as t.Node[], scope), onClick: p.get("onClick") });
  }
  return out;
}

/** Emit the T.Menu object lines (opening at `level`). `anchorId` is the Item the CSS ancestor
 *  walk re-anchors at (and whose classes scope `.popup`/`.option` rules); extra classes from the
 *  author's `class` prop are merged into the popup background's cssClass. */
function menuObjectLines(opts: {
  menuId: string;
  anchorId: string;
  items: MenuChild[];
  classes: string[];
  title?: string;       // MenuBar submenu title (already a QML string expression)
  xExpr?: string;
  yExpr?: string;
  onCloseBody?: string;
}, scope: Scope, level: number): string[] {
  const i = (n: number) => INDENT.repeat(level + n);
  const { menuId, anchorId, items, classes } = opts;
  const counter = scope.inputCounter ?? { n: 0 };
  if (scope.usedWidgets) { scope.usedWidgets.flag = true; scope.usedWidgets.popupWindow = true; }

  const popupClass = [...classes, "popup"].map((c) => JSON.stringify(c)).join(", ");

  const lines: string[] = [
    `${i(0)}T.Menu {`,
    `${i(1)}id: ${menuId}`,
    ...(opts.title ? [`${i(1)}title: ${opts.title}`] : []),
    ...(opts.xExpr ? [`${i(1)}x: ${opts.xExpr}`] : []),
    ...(opts.yExpr ? [`${i(1)}y: ${opts.yExpr}`] : []),
    // Desktop menu: a REAL native popup window (Qt 6.8+), like the <select> dropdown.
    `${i(1)}popupType: T.Popup.Window`,
    // Templates popups have NO implicit-size policy — this is the style's sizing formula:
    // without it the menu opens 0x0. The width floor lives HERE, not on the background:
    // a background CssFill's implicitWidth is overwritten by the CSS engine's content pass
    // (it hosts no Css layout children → measures 0), so Basic's implicitBackgroundWidth
    // trick reads 0 under our engine (probed: the menu opened 2px wide).
    `${i(1)}implicitWidth: Math.max(180, implicitContentWidth + leftPadding + rightPadding)`,
    `${i(1)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
    // padding ≥ border-width prevents the popup CssFill border from clipping rows (G3).
    `${i(1)}padding: 1`,
    ...(opts.onCloseBody ? [`${i(1)}onClosed: { ${opts.onCloseBody} }`] : []),
    // cssAncestor: popup contents reparent to the window Overlay — re-anchor the CSS walk.
    // background and contentItem are SIBLING slots; every menu descendant passes through one.
    `${i(1)}background: Css.CssFill {`,
    `${i(2)}property Item cssAncestor: ${anchorId}`,
    `${i(2)}cssPrimitive: "div"`,
    `${i(2)}cssClass: [${popupClass}]`,
    `${i(1)}}`,
    `${i(1)}contentItem: ListView {`,
    `${i(2)}property Item cssAncestor: ${anchorId}`,
    `${i(2)}clip: true`,
    `${i(2)}model: ${menuId}.contentModel`,
    `${i(2)}currentIndex: ${menuId}.currentIndex`,
    `${i(2)}implicitHeight: contentHeight`,
    `${i(1)}}`,
  ];

  for (const child of items) {
    if (child.kind === "separator") {
      lines.push(
        `${i(1)}T.MenuSeparator {`,
        `${i(2)}implicitWidth: 180`,
        `${i(2)}implicitHeight: 9`,
        `${i(2)}padding: 4`,
        `${i(2)}contentItem: Css.CssRect {`,
        `${i(3)}cssPrimitive: "div"`,
        `${i(3)}cssClass: ["sep"]`,
        `${i(3)}implicitHeight: 1`,
        `${i(2)}}`,
        `${i(1)}}`,
      );
      continue;
    }
    const itemId = `__mitem${counter.n++}`;
    const clickBody = child.onClick ? handlerBody(child.onClick, scope) : "";
    lines.push(
      `${i(1)}T.MenuItem {`,
      `${i(2)}id: ${itemId}`,
      // The menu highlights the hovered/keyboard-current row via `highlighted`; hoverEnabled
      // makes mouse rows current (Controls styles rely on the same).
      `${i(2)}hoverEnabled: true`,
      `${i(2)}implicitWidth: Math.max(180, implicitContentWidth + leftPadding + rightPadding)`,
      `${i(2)}implicitHeight: 32`,
      `${i(2)}leftPadding: 12`,
      `${i(2)}rightPadding: 12`,
      `${i(2)}text: ${child.labelBinding ?? '""'}`,
      ...(clickBody ? [`${i(2)}onTriggered: { ${clickBody} }`] : []),
      `${i(2)}background: Css.CssFill {`,
      `${i(3)}cssPrimitive: "div"`,
      `${i(3)}cssClass: ["option"]`,
      `${i(3)}cssState: ${itemId}.highlighted ? ["hover"] : []`,
      `${i(2)}}`,
      `${i(2)}contentItem: Css.CssText {`,
      `${i(3)}cssPrimitive: ""`,
      `${i(3)}cssClass: ["option-label"]`,
      `${i(3)}text: ${itemId}.text`,
      `${i(2)}}`,
      `${i(1)}}`,
    );
  }

  lines.push(`${i(0)}}`);
  return lines;
}

// ─── <Menu open={sig()} x={..} y={..} onClose={..}> — standalone (context/action menu) ──────────

/** The menu is hosted in a zero-size Item: T.Menu is a popup (not an Item), the host gives it a
 *  stable position in the tree, an anchor for the CSS ancestor walk, and a place for the
 *  Qt::Popup deactivation idiom (Window attached property needs an Item). Controlled open state
 *  uses the emitInput Binding idiom: a Binding element (restoreMode RestoreNone) re-asserts the
 *  signal into `visible` without being destroyed when the menu closes itself (Esc/click-outside);
 *  onClosed → the author's onClose keeps the signal honest. */
function emitMenu(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const hostId = `__menuHost${n}`;
  const menuId = `__menu${n}`;

  const props = propsOf(propsArg);
  const binding = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });
  const openProp = props.get("open");
  const xProp = props.get("x");
  const yProp = props.get("y");
  const onClose = props.get("onClose");

  const items = parseMenuChildren(children, scope, "Menu");

  const lines: string[] = [
    `${pad}Item {`,
    `${i(1)}id: ${hostId}`,
    `${i(1)}width: 0`,
    `${i(1)}height: 0`,
    // Qt::Popup semantics (owner directive, see emitSelect): a native popup window must not
    // linger over other applications when this one deactivates.
    `${i(1)}Window.onActiveChanged: if (!Window.active) ${menuId}.close()`,
    ...menuObjectLines({
      menuId, anchorId: hostId, items, classes: classesOf(props),
      xExpr: xProp ? binding(xProp) : undefined,
      yExpr: yProp ? binding(yProp) : undefined,
      onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
    }, scope, level + 1),
  ];

  // Controlled open: Binding element (survives the menu closing itself, unlike a plain
  // `visible:` binding which self-close would break). A Show guard folds into the value —
  // a popup must never be opened by `visible: !!(guard)` alone.
  if (openProp) {
    const value = `!!(${binding(openProp)})${guard ? ` && !!(${guard})` : ""}`;
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${menuId}`,
      `${i(2)}property: "visible"`,
      `${i(2)}value: ${value}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

// ─── <MenuBar> with <Menu title="…"> children ───────────────────────────────────────────────────

/** T.MenuBar is a QQuickContainer. Canonical Basic-style structure: contentItem Row over
 *  `menuBar.contentModel`. Each child <Menu title> becomes an EXPLICIT T.MenuBarItem declared in
 *  contentData — qquickmenubar.cpp appends MenuBarItems directly ("you can add MenuBarItems
 *  directly to the menu bar"), wiring hover-open/click-open — with the submenu attached via its
 *  `menu` property (an object binding, so contentData_append never sees a bare Menu and never
 *  creates a duplicate delegate item). */
function emitMenuBar(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const barId = `__mbar${n}`;
  const classLine = buildCssClassLine(cssPropsShim(propsArg), scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.flag = true;

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "div"`,
    // Best-effort defaults: once the author's CSS gives the wrapper box rules, the engine's
    // content pass measures the plain T.MenuBar as 0 and OVERWRITES these — size the box in
    // CSS then (canon: .wg-cal/.wg-date in examples/widgets.css do exactly this).
    `${i(1)}implicitWidth: ${barId}.implicitWidth`,
    `${i(1)}implicitHeight: ${barId}.implicitHeight`,
    `${i(1)}T.MenuBar {`,
    `${i(2)}id: ${barId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)`,
    `${i(2)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
    `${i(2)}contentItem: Row {`,
    `${i(3)}spacing: ${barId}.spacing`,
    `${i(3)}Repeater { model: ${barId}.contentModel }`,
    `${i(2)}}`,
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["menubar"]`,
    `${i(2)}}`,
  ];

  for (const child of children) {
    if (!isHCall(child)) continue;
    const { tag, props: menuProps, children: menuKids } = hParts(child as t.CallExpression);
    const name = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : "?";
    if (name !== "Menu") throw new Error(`<MenuBar> only accepts <Menu title="…"> children, got <${name}>`);
    const p = propsOf(menuProps);
    const titleExpr = p.get("title");
    const title = titleExpr
      ? (t.isStringLiteral(titleExpr) ? JSON.stringify(titleExpr.value) : emitExpr(titleExpr, { ...scope, mode: "binding" }))
      : '""';
    const itemId = `__mbi${counter.n++}`;
    const menuId = `__menu${counter.n++}`;
    const items = parseMenuChildren(menuKids as t.Node[], scope, "Menu");
    const menuLines = menuObjectLines({ menuId, anchorId: itemId, items, classes: classesOf(p), title }, scope, level + 3);
    // Re-shape the submenu as the `menu:` object binding of the MenuBarItem.
    menuLines[0] = `${i(3)}menu: ${menuLines[0].trimStart()}`;
    lines.push(
      `${i(2)}T.MenuBarItem {`,
      `${i(3)}id: ${itemId}`,
      `${i(3)}hoverEnabled: true`,
      `${i(3)}implicitWidth: implicitContentWidth + leftPadding + rightPadding`,
      `${i(3)}implicitHeight: implicitContentHeight + topPadding + bottomPadding`,
      `${i(3)}leftPadding: 12`,
      `${i(3)}rightPadding: 12`,
      `${i(3)}topPadding: 6`,
      `${i(3)}bottomPadding: 6`,
      // Qt::Popup semantics for the drop-down (see emitMenu).
      `${i(3)}Window.onActiveChanged: if (!Window.active && ${itemId}.menu) ${itemId}.menu.close()`,
      ...menuLines,
      `${i(3)}background: Css.CssFill {`,
      `${i(4)}cssPrimitive: "div"`,
      `${i(4)}cssClass: ["menubar-item"]`,
      // `highlighted` = this item's menu is the open one (set by the menu bar).
      `${i(4)}cssState: (${itemId}.hovered ? ["hover"] : []).concat(${itemId}.highlighted ? ["open"] : [])`,
      `${i(3)}}`,
      `${i(3)}contentItem: Css.CssText {`,
      `${i(4)}cssPrimitive: ""`,
      `${i(4)}cssClass: ["menubar-label"]`,
      `${i(4)}text: ${itemId}.menu ? ${itemId}.menu.title : ""`,
      `${i(3)}}`,
      `${i(2)}}`,
    );
  }

  lines.push(`${i(1)}}`, `${pad}}`);
  return lines;
}

// ─── <TreeView data={expr} onSelect={(node) => …}> — our own recursive tree ────────────────────

/** `data` contract: an array of plain { label, children? } objects. Every node renders a
 *  full-width `.tree-row` (indentation is depth*16 INSIDE the row, so hover/selection paint
 *  edge-to-edge like every desktop tree); children stack under it via a Repeater when expanded.
 *
 *  NOT virtualized: rows are real items. Qt's own TreeView needs a QAbstractItemModel and brings
 *  recycling — that is the Phase 4 virtualization plan; this covers sidebar/config-panel trees. */
function emitTreeView(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const treeId = `__tree${n}`;
  const compId = `__treeComp${n}`;
  const colId = `__treeCol${n}`;
  const nodeId = `__tnode${n}`;
  const maId = `__trowMa${n}`;
  const selFn = `__treeSel${n}`;

  const props = propsOf(propsArg);
  const classLine = buildCssClassLine(cssPropsShim(propsArg), scope, i(1));
  const dataProp = props.get("data");
  const dataExpr = dataProp ? emitExpr(dataProp, { ...scope, mode: "binding" }) : "[]";
  const onSelect = props.get("onSelect");
  const selBody = onSelect ? handlerBody(onSelect, scope, "__node") : "";

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}id: ${treeId}`,
    `${i(1)}cssPrimitive: "div"`,
    // Best-effort defaults for a bare (CSS-less) tree. With box rules on the wrapper, the
    // engine's content pass measures the plain anchored host as 0 and OVERWRITES these —
    // author CSS must size the pane (e.g. `.nv-tree { width; height }`), which is also the
    // desktop-correct shape: trees live in fixed panes, they don't grow the page.
    `${i(1)}implicitWidth: 240`,
    `${i(1)}implicitHeight: ${colId}.height`,
    ...(selBody ? [`${i(1)}function ${selFn}(__node) { ${selBody} }`] : []),
    // Anchored plain-Item host: insulates the tree internals from the wrapper's CSS layout pass
    // (same pattern as <Calendar> / the date input — anchored plain items are skipped).
    `${i(1)}Item {`,
    `${i(2)}anchors.fill: parent`,
    // Recursive node component. The delegate references the Component BY ID — the only legal
    // self-recursion in a single QML file (a self-referencing inline component is a compile
    // error: "Inline components form a cycle!"). Depth travels through the model rows
    // ({ __n: node, __d: depth }) because delegate contexts resolve at the Component's
    // DECLARATION site, not its instantiation site.
    `${i(2)}Component {`,
    `${i(3)}id: ${compId}`,
    `${i(3)}Column {`,
    `${i(4)}id: ${nodeId}`,
    `${i(4)}width: parent ? parent.width : 0`,
    `${i(4)}property var node: modelData.__n`,
    `${i(4)}property int depth: modelData.__d`,
    `${i(4)}property bool expanded: true`,
    `${i(4)}Css.CssFill {`,
    `${i(5)}cssPrimitive: "div"`,
    `${i(5)}cssClass: ["tree-row"]`,
    `${i(5)}cssState: ${maId}.containsMouse ? ["hover"] : []`,
    `${i(5)}width: ${nodeId}.width`,
    `${i(5)}height: 28`,
    `${i(5)}implicitHeight: 28`,
    // Anchored host for the row internals — plain items inside a Css container MUST be hosted
    // this way (see the checkbox indicator comment in qml.ts) or a CSS box rule on .tree-row
    // triggers a flex pass that stretches them.
    `${i(5)}Item {`,
    `${i(6)}anchors.fill: parent`,
    `${i(6)}Text {`,
    `${i(7)}x: 8 + ${nodeId}.depth * 16`,
    `${i(7)}anchors.verticalCenter: parent.verticalCenter`,
    `${i(7)}text: (${nodeId}.node && ${nodeId}.node.children && ${nodeId}.node.children.length) ? (${nodeId}.expanded ? "▾" : "▸") : ""`,
    // CssItem injection styles the plain disclosure glyph (colour/font) without joining a layout.
    `${i(7)}Css.CssItem { cssPrimitive: "text"; cssClass: ["tree-disclosure"] }`,
    `${i(6)}}`,
    `${i(6)}Css.CssText {`,
    `${i(7)}cssPrimitive: ""`,
    `${i(7)}cssClass: ["tree-label"]`,
    `${i(7)}x: 8 + ${nodeId}.depth * 16 + 18`,
    `${i(7)}anchors.verticalCenter: parent.verticalCenter`,
    `${i(7)}text: ${nodeId}.node ? ("" + ${nodeId}.node.label) : ""`,
    `${i(6)}}`,
    `${i(6)}MouseArea {`,
    `${i(7)}id: ${maId}`,
    `${i(7)}anchors.fill: parent`,
    `${i(7)}hoverEnabled: true`,
    `${i(7)}cursorShape: Qt.PointingHandCursor`,
    `${i(7)}onClicked: { ${nodeId}.expanded = !${nodeId}.expanded${selBody ? `; ${treeId}.${selFn}(${nodeId}.node)` : ""} }`,
    `${i(6)}}`,
    `${i(5)}}`,
    `${i(4)}}`,
    `${i(4)}Repeater {`,
    `${i(5)}model: (${nodeId}.expanded && ${nodeId}.node && ${nodeId}.node.children) ? ${nodeId}.node.children.map(function(c) { return ({ __n: c, __d: ${nodeId}.depth + 1 }) }) : []`,
    `${i(5)}delegate: ${compId}`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
    `${i(2)}Column {`,
    `${i(3)}id: ${colId}`,
    `${i(3)}width: parent.width`,
    `${i(3)}Repeater {`,
    `${i(4)}model: ((${dataExpr}) || []).map(function(c) { return ({ __n: c, __d: 0 }) })`,
    `${i(4)}delegate: ${compId}`,
    `${i(3)}}`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${pad}}`,
  ];
}

// ─── <Tray tooltip onActivate icon> with optional <MenuItem> children ───────────────────────────

/** Platform.SystemTrayIcon is a QObject, NOT an Item — the zero-size Item host lets the tag sit
 *  anywhere in the tree (and a Show guard folds into the icon's own `visible`, since Item
 *  visibility does not cascade to non-Item resources). */
function emitTray(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const trayId = `__tray${n}`;

  requireImport(scope, "import Qt.labs.platform 1.1 as Platform");

  const props = propsOf(propsArg);
  const binding = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });
  const tooltip = props.get("tooltip");
  const icon = props.get("icon");
  const onActivate = props.get("onActivate");

  const lines: string[] = [
    `${pad}Item {`,
    `${i(1)}width: 0`,
    `${i(1)}height: 0`,
    `${i(1)}Platform.SystemTrayIcon {`,
    `${i(2)}id: ${trayId}`,
    `${i(2)}visible: ${guard ? `!!(${guard})` : "true"}`,
    ...(tooltip ? [`${i(2)}tooltip: ${binding(tooltip)}`] : []),
    ...(icon ? [`${i(2)}icon.source: ${binding(icon)}`] : []),
    ...(onActivate ? [`${i(2)}onActivated: { ${handlerBody(onActivate, scope)} }`] : []),
  ];

  const items = parseMenuChildren(children, scope, "Tray");
  if (items.length > 0) {
    lines.push(`${i(2)}menu: Platform.Menu {`);
    for (const item of items) {
      if (item.kind === "separator") { lines.push(`${i(3)}Platform.MenuItem { separator: true }`); continue; }
      const clickBody = item.onClick ? handlerBody(item.onClick, scope) : "";
      lines.push(
        `${i(3)}Platform.MenuItem {`,
        `${i(4)}text: ${item.labelBinding ?? '""'}`,
        ...(clickBody ? [`${i(4)}onTriggered: { ${clickBody} }`] : []),
        `${i(3)}}`,
      );
    }
    lines.push(`${i(2)}}`);
  }

  lines.push(`${i(1)}}`, `${pad}}`);
  return lines;
}

// ─── registration ───────────────────────────────────────────────────────────────────────────────

registerNativeTags({
  Menu: emitMenu,
  MenuBar: emitMenuBar,
  TreeView: emitTreeView,
  Tray: emitTray,
  // Only meaningful as children of <Menu>/<MenuBar>/<Tray>; a stray one is an authoring error.
  MenuItem: () => { throw new Error("<MenuItem> must be a child of <Menu> or <Tray>"); },
  MenuSeparator: () => { throw new Error("<MenuSeparator> must be a child of <Menu>"); },
});
