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
import { buildCssClassLine, emitChildren, guardLine, INDENT } from "../qml.ts";
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

/** Mark that a solidqml.Widgets component was instantiated → prepend `import solidqml.Widgets`. */
function markWidgetLib(scope: Scope): void {
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
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
  debounce?: boolean;   // record __closedAt on close so a trigger can swallow the dismissing click
}, scope: Scope, level: number): string[] {
  const i = (n: number) => INDENT.repeat(level + n);
  const { menuId, anchorId, items, classes } = opts;
  // A close-then-open race (clicking the trigger while open: press-outside closes, click reopens)
  // is suppressed by recording when the menu last closed; the trigger ignores an open() within the
  // debounce window (QToolButton+QMenu idiom). Fold any author onClose in alongside the timestamp.
  const closedBody = opts.debounce
    ? `__closedAt = Date.now();${opts.onCloseBody ? ` ${opts.onCloseBody}` : ""}`
    : opts.onCloseBody;
  const counter = scope.inputCounter ?? { n: 0 };
  if (scope.usedWidgets) { scope.usedWidgets.flag = true; scope.usedWidgets.popupWindow = true; }

  const popupClass = [...classes, "popup"].map((c) => JSON.stringify(c)).join(", ");

  const lines: string[] = [
    `${i(0)}T.Menu {`,
    `${i(1)}id: ${menuId}`,
    ...(opts.debounce ? [`${i(1)}property double __closedAt: 0`] : []),
    ...(opts.title ? [`${i(1)}title: ${opts.title}`] : []),
    ...(opts.xExpr ? [`${i(1)}x: ${opts.xExpr}`] : []),
    ...(opts.yExpr ? [`${i(1)}y: ${opts.yExpr}`] : []),
    // In-scene overlay popup (NOT Popup.Window): Wayland compositors don't honor client toplevel
    // positioning, so a window-type menu opens at the top of the screen once the app floats. An
    // Item popup opens relative to its host in the scene (same fix as <select>/<input type=date>).
    `${i(1)}popupType: T.Popup.Item`,
    // Templates popups have NO implicit-size policy — this is the style's sizing formula:
    // without it the menu opens 0x0. The width floor lives HERE, not on the background:
    // a background CssFill's implicitWidth is overwritten by the CSS engine's content pass
    // (it hosts no Css layout children → measures 0), so Basic's implicitBackgroundWidth
    // trick reads 0 under our engine (probed: the menu opened 2px wide).
    `${i(1)}implicitWidth: Math.max(180, implicitContentWidth + leftPadding + rightPadding)`,
    `${i(1)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
    // padding ≥ border-width prevents the popup CssFill border from clipping rows (G3).
    `${i(1)}padding: 1`,
    ...(closedBody ? [`${i(1)}onClosed: { ${closedBody} }`] : []),
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
      // Desktop affordance: a pointing-hand cursor over the row (HoverHandler doesn't steal the
      // press, so highlight/activation still work through it).
      `${i(2)}HoverHandler { cursorShape: Qt.PointingHandCursor }`,
      `${i(2)}background: Css.CssFill {`,
      `${i(3)}cssPrimitive: "div"`,
      `${i(3)}cssClass: ["option"]`,
      `${i(3)}cssState: ${itemId}.highlighted ? ["hover"] : []`,
      `${i(2)}}`,
      `${i(2)}contentItem: Css.CssText {`,
      `${i(3)}cssPrimitive: ""`,
      `${i(3)}cssClass: ["option-label"]`,
      // Mnemonic marker: `&N` underlines/accelerates N on desktop — strip the `&` for display
      // (a literal ampersand is written `&&`). Functional Alt+letter activation is the keyboard
      // model pass (see the tab-focus study).
      `${i(3)}text: ${itemId}.text.replace(/&(.)/g, "$1")`,
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
  const triggerProp = props.get("trigger");

  const items = parseMenuChildren(children, scope, "Menu");

  // ── Self-managed form: <Menu trigger={<button>…</button>}> (owner directive 2026-07-05) ──
  // The menu owns its open/close; the embedded trigger TOGGLES it. Clicking the trigger while the
  // menu is open closes it and does NOT reopen — the __closedAt debounce swallows the same click
  // that press-outside used to dismiss the popup (the QToolButton+QMenu idiom). No external signal.
  if (triggerProp) {
    if (!t.isExpression(triggerProp) || !isHCall(triggerProp))
      throw new Error("<Menu trigger={…}> expects a single element (e.g. a <button>)");
    const trigId = `__mtrig${n}`;
    // Emit the trigger (a <button> → W.CssButton) and wire the toggle to its `onClicked` signal.
    // Give its root a stable id for sizing/positioning the menu below it.
    const trig = emitChildren([triggerProp], scope, level + 1);
    const openIdx = trig.findIndex((l) => /\{\s*$/.test(l));
    if (openIdx < 0) throw new Error("<Menu trigger> did not emit an element");
    if (!/\bW\.Button \{\s*$/.test(trig[openIdx]))
      throw new Error("<Menu trigger> must be a <button>");
    if (trig.some((l) => /^\s*onClicked:/.test(l)))
      throw new Error("<Menu trigger> button must not declare its own onClick");
    const toggle = `if (${menuId}.visible) ${menuId}.close(); else if (Date.now() - ${menuId}.__closedAt > 250) ${menuId}.open()`;
    trig.splice(openIdx + 1, 0, `${i(2)}id: ${trigId}`, `${i(2)}onClicked: { ${toggle} }`);

    return [
      `${pad}Item {`,
      `${i(1)}id: ${hostId}`,
      ...(guard ? [`${i(1)}visible: !!(${guard})`] : []),
      // Size the host to the trigger's implicit (content) size — not childrenRect, which would
      // cycle against the button's fill. The popup is not a visual child, so it doesn't count.
      `${i(1)}implicitWidth: ${trigId}.implicitWidth`,
      `${i(1)}implicitHeight: ${trigId}.implicitHeight`,
      `${i(1)}Window.onActiveChanged: if (!Window.active) ${menuId}.close()`,
      ...trig,
      ...menuObjectLines({
        menuId, anchorId: hostId, items, classes: classesOf(props),
        yExpr: `${hostId}.height + 2`,
        onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
        debounce: true,
      }, scope, level + 1),
      `${pad}}`,
    ];
  }

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

/** `data` contract: an array of plain { label, children? } objects. One .qml per component
 *  (owner directive 2026-07-05): the recursive-node structure — full-width `.tree-row`s, the
 *  self-referencing Component, the disclosure glyph and depth-indented label — all live in
 *  TreeView.qml. The emit is thin: it passes the `data` array as `treeData` and wires the
 *  author's onSelect to the component's `selected(node)` signal (Item's default property is
 *  named `data`, so the node array is exposed as `treeData`). */
function emitTreeView(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);

  const props = propsOf(propsArg);
  const classLine = buildCssClassLine(cssPropsShim(propsArg), scope, i(1));
  const dataProp = props.get("data");
  const dataExpr = dataProp ? emitExpr(dataProp, { ...scope, mode: "binding" }) : "[]";
  const onSelect = props.get("onSelect");
  const selBody = onSelect ? handlerBody(onSelect, scope, "node") : "";

  const lines: string[] = [
    `${pad}W.TreeView {`,
    ...classLine,
    ...guardLine(guard, level),
    // `__treeData` (not `treeData`): a self-named author binding — the example's own `treeData` —
    // would resolve to the component's own property (`treeData: treeData` = self), so the wiring
    // channel is `__`-prefixed to force the RHS to resolve to the outer identifier.
    `${i(1)}__treeData: ${dataExpr}`,
  ];
  if (selBody) lines.push(`${i(1)}onSelected: (node) => { ${selBody} }`);
  lines.push(`${pad}}`);
  return lines;
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
