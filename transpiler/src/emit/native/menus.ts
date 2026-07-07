// Native menus group: <Menu>/<MenuItem>/<MenuSeparator>, <MenuBar>, <TreeView>, <Tray>.
//
// Owner directive 2026-07-05: every widget is a .qml in solidqml.Widgets; this emitter INSTANTIATES
// them and keeps only the orchestration. Each widget's SHELL lives in qml/solidqml/Widgets/:
//   - W.Menu (Menu.qml): the T.Menu popup — popupType Popup.Item (Wayland-correct in-scene
//     positioning; a Popup.Window opens at the screen top once the app floats), the style's implicit
//     size (Templates popups have no size policy — we ARE the style → 0x0 otherwise), the .popup
//     background + contentItem ListView with the cssAncestor re-anchor on BOTH sibling slots (popup
//     contents reparent to the Overlay, severing the CSS walk), and the __closedAt debounce store.
//   - W.MenuItem / W.MenuSeparator: the .option/.option-label rows (cursor + `&`-mnemonic strip) and
//     the 1px .sep. W.MenuBar / W.MenuBarItem: the T.MenuBar Basic-style chrome + the entry slots.
//   - W.Tray (Tray.qml): the zero-size Item host + SystemTrayIcon shell (a QObject, NOT an Item).
//   - W.TreeView (TreeView.qml): OUR OWN recursive tree over plain {label, children?} objects (Qt's
//     TreeView needs a QAbstractItemModel + recycling — Phase 4 virtualization).
//
// The emit KEEPS: which Menu form (standalone controlled `open` Binding vs self-managed `trigger`
// toggle + __closedAt debounce), the trigger injection, the Window-deactivation close, item/separator
// parsing, the MenuBar submenu wiring, and — for Tray — the Platform.Menu construction (author item
// handlers), assigned through the component's `menu` alias so registration stays identical.
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

/** Emit the W.Menu instance lines (opening at `level`). One .qml per component (owner directive
 *  2026-07-05): the T.Menu shell (popupType/implicit-size/padding/cssAncestor slots/background/
 *  contentItem/__closedAt) lives in Menu.qml; each item is a W.MenuItem / W.MenuSeparator (their own
 *  components). This emit only wires the instance: the CSS anchor (`cssAncestor`), the author's
 *  merge-classes (`authorClass`, concatenated with "popup" inside the component), an optional
 *  MenuBar-submenu `title`, positioning `x`/`y`, the author's onClose (via the component's
 *  `menuClosed` signal — the internal onClosed is taken by the __closedAt debounce record), and the
 *  item children. `anchorId` is the host Item the CSS walk re-anchors at (popup contents reparent to
 *  the Overlay, severing the visual chain). */
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
  markWidgetLib(scope);

  const authorClass = classes.map((c) => JSON.stringify(c)).join(", ");

  const lines: string[] = [
    `${i(0)}W.Menu {`,
    `${i(1)}id: ${menuId}`,
    `${i(1)}cssAncestor: ${anchorId}`,
    ...(classes.length ? [`${i(1)}authorClass: [${authorClass}]`] : []),
    ...(opts.title ? [`${i(1)}title: ${opts.title}`] : []),
    ...(opts.xExpr ? [`${i(1)}x: ${opts.xExpr}`] : []),
    ...(opts.yExpr ? [`${i(1)}y: ${opts.yExpr}`] : []),
    ...(opts.onCloseBody ? [`${i(1)}onMenuClosed: { ${opts.onCloseBody} }`] : []),
  ];

  for (const child of items) {
    if (child.kind === "separator") { lines.push(`${i(1)}W.MenuSeparator { }`); continue; }
    const clickBody = child.onClick ? handlerBody(child.onClick, scope) : "";
    lines.push(
      `${i(1)}W.MenuItem {`,
      `${i(2)}text: ${child.labelBinding ?? '""'}`,
      ...(clickBody ? [`${i(2)}onTriggered: { ${clickBody} }`] : []),
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

  markWidgetLib(scope);
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
      // The host must be a Css BOX, not a plain Item: the layout engine only flows Css children,
      // so a plain host inside a row with other Css siblings is invisible to flex and the sibling
      // after it overlaps the trigger. As a class-less "div" the engine measures its implicit
      // from the button; the popup is a non-visual object child and doesn't count.
      `${pad}Css.CssRect {`,
      `${i(1)}id: ${hostId}`,
      `${i(1)}cssPrimitive: "div"`,
      ...(guard ? [`${i(1)}visible: !!(${guard})`] : []),
      `${i(1)}Window.onActiveChanged: if (!Window.active) ${menuId}.close()`,
      ...trig,
      ...menuObjectLines({
        menuId, anchorId: hostId, items, classes: classesOf(props),
        yExpr: `${hostId}.height + 2`,
        onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
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

/** One .qml per component (owner directive 2026-07-05): the CssFill "div" wrapper, the T.MenuBar
 *  (Row+Repeater contentItem over contentModel, `.menubar` background) live in MenuBar.qml, and each
 *  top-level entry's chrome (`.menubar-item`/`.menubar-label` slots, Window-deactivation close) in
 *  MenuBarItem.qml. The emit is thin: it wires the author classes and, per <Menu title> child, a
 *  W.MenuBarItem whose `menu` is the W.Menu submenu (an object binding; the submenu re-anchors its
 *  CSS walk at the MenuBarItem's id). The children route into the bar's contentData via the default
 *  `barItems` alias. */
function emitMenuBar(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  // OS-native menu bar (owner call 2026-07-07): a Qt.labs.platform.MenuBar attaches to the window
  // chrome (needs Qt Widgets, which the loader now links + a QApplication). It is a QObject, not a
  // visual — like <Tray>, it takes no layout slot and carries no CSS (it's OS chrome, not scene). Its
  // <Menu title>/<MenuItem>/<MenuSeparator> children map straight to the native Platform types.
  requireImport(scope, "import Qt.labs.platform 1.1 as Platform");

  const lines: string[] = [`${pad}Platform.MenuBar {`];
  if (guard) lines.push(`${i(1)}enabled: !!(${guard})`);

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
    const items = parseMenuChildren(menuKids as t.Node[], scope, "Menu");
    lines.push(`${i(1)}Platform.Menu {`, `${i(2)}title: ${title}`);
    for (const item of items) {
      if (item.kind === "separator") { lines.push(`${i(2)}Platform.MenuSeparator { }`); continue; }
      const clickBody = item.onClick ? handlerBody(item.onClick, scope) : "";
      lines.push(
        `${i(2)}Platform.MenuItem {`,
        `${i(3)}text: ${item.labelBinding ?? '""'}`,
        ...(clickBody ? [`${i(3)}onTriggered: { ${clickBody} }`] : []),
        `${i(2)}}`,
      );
    }
    lines.push(`${i(1)}}`);
  }

  lines.push(`${pad}}`);
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

/** One .qml per component (owner directive 2026-07-05): the zero-size Item host + the
 *  SystemTrayIcon shell (visible/tooltip/iconSource/activated) live in Tray.qml. The emit stays
 *  thin — it wires those props and, ONLY when the author gave <MenuItem> children, constructs the
 *  Platform.Menu (carrying the item handlers) and assigns it through the component's `menu` alias.
 *  No items → no menu is set, so the SystemTrayIcon registration is byte-identical to before. */
function emitTray(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);
  requireImport(scope, "import Qt.labs.platform 1.1 as Platform");

  const props = propsOf(propsArg);
  const binding = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });
  const tooltip = props.get("tooltip");
  const icon = props.get("icon");
  const onActivate = props.get("onActivate");

  const lines: string[] = [`${pad}W.Tray {`];
  if (guard) lines.push(`${i(1)}shown: !!(${guard})`);
  if (tooltip) lines.push(`${i(1)}tooltip: ${binding(tooltip)}`);
  if (icon) lines.push(`${i(1)}iconSource: ${binding(icon)}`);
  if (onActivate) lines.push(`${i(1)}onActivated: { ${handlerBody(onActivate, scope)} }`);

  const items = parseMenuChildren(children, scope, "Tray");
  if (items.length > 0) {
    lines.push(`${i(1)}menu: Platform.Menu {`);
    for (const item of items) {
      if (item.kind === "separator") { lines.push(`${i(2)}Platform.MenuItem { separator: true }`); continue; }
      const clickBody = item.onClick ? handlerBody(item.onClick, scope) : "";
      lines.push(
        `${i(2)}Platform.MenuItem {`,
        `${i(3)}text: ${item.labelBinding ?? '""'}`,
        ...(clickBody ? [`${i(3)}onTriggered: { ${clickBody} }`] : []),
        `${i(2)}}`,
      );
    }
    lines.push(`${i(1)}}`);
  }

  lines.push(`${pad}}`);
  return lines;
}

// ─── <ListView data={…} onSelect={(item, index) => …}> — a real virtualized QtQuick ListView ──────

/** One .qml per component: the QtQuick ListView + `.list-item` delegate live in ListView.qml; this
 *  emit wires the author classes, the `data` array (as `__listData` — same self-reference guard as
 *  TreeView's `__treeData`), and onSelect → the component's `selected(item, index)` signal. */
function emitListView(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);
  const props = propsOf(propsArg);
  const classLine = buildCssClassLine(cssPropsShim(propsArg), scope, i(1));
  const dataProp = props.get("data");
  const dataExpr = dataProp ? emitExpr(dataProp, { ...scope, mode: "binding" }) : "[]";
  const onSelect = props.get("onSelect");
  const selBody = onSelect ? handlerBody(onSelect, scope, "item") : "";

  const lines: string[] = [
    `${pad}W.ListView {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}__listData: ${dataExpr}`,
  ];
  // The QML signal is selected(item, index); the author's first param maps to `item`.
  if (selBody) lines.push(`${i(1)}onSelected: (item, index) => { ${selBody} }`);
  lines.push(`${pad}}`);
  return lines;
}

// ─── <TableView columns={…} data={…} onSelect={(row, index) => …}> — header + virtualized rows ────

/** columns is `[{key,label}]` (or bare strings); data is an array of row objects. The header + row
 *  ListView + `.table-*` slots live in TableView.qml; this emit wires classes, `__columns`/`__rows`,
 *  and onSelect → the component's selected(row, index) signal (author's first param maps to `row`). */
function emitTableView(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);
  const props = propsOf(propsArg);
  const classLine = buildCssClassLine(cssPropsShim(propsArg), scope, i(1));
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });
  const columns = props.get("columns");
  const data = props.get("data");
  const onSelect = props.get("onSelect");
  const selBody = onSelect ? handlerBody(onSelect, scope, "row") : "";

  const lines: string[] = [
    `${pad}W.TableView {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}__columns: ${columns ? bind(columns) : "[]"}`,
    `${i(1)}__rows: ${data ? bind(data) : "[]"}`,
  ];
  if (selBody) lines.push(`${i(1)}onSelected: (row, index) => { ${selBody} }`);
  lines.push(`${pad}}`);
  return lines;
}

// ─── registration ───────────────────────────────────────────────────────────────────────────────

registerNativeTags({
  Menu: emitMenu,
  MenuBar: emitMenuBar,
  TreeView: emitTreeView,
  ListView: emitListView,
  TableView: emitTableView,
  Tray: emitTray,
  // Only meaningful as children of <Menu>/<MenuBar>/<Tray>; a stray one is an authoring error.
  MenuItem: () => { throw new Error("<MenuItem> must be a child of <Menu> or <Tray>"); },
  MenuSeparator: () => { throw new Error("<MenuSeparator> must be a child of <Menu>"); },
});
