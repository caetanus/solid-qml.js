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
import { boolAttrExpr, buildCssClassLine, emitChildren, enabledFromDisabled, guardLine, INDENT } from "../qml.ts";
import { checkAttrs, rejectForKey } from "../attrs.ts";
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

/** Used only by <Tray> (its items become Platform.MenuItem, a separate object system — see
 *  emitTray) — a flat MenuItem/MenuSeparator list, no <For>/<Show>. <Menu>/<ContextMenu> (the
 *  W.Menu-backed forms) go through emitMenuItemLines below instead, which also accepts <For>/
 *  <Show>. */
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

// ─── <Menu>/<ContextMenu> item body: <MenuItem>/<MenuSeparator> plus <For>/<Show> ──────────────
//
// Qt's own docs (Menu QML Type, "Performance considerations when dynamically populating menus")
// recommend `Instantiator`, with `onObjectAdded`/`onObjectRemoved` explicitly calling
// `menu.insertItem(index, object)` / `menu.removeItem(object)` — see the reference snippet
// src/quickcontrols/doc/snippets/qtquickcontrols-menu-instantiator.qml in the Qt sources — rather
// than a bare `Repeater` child. We measured why: QQuickMenu::contentData()'s DEFAULT-PROPERTY
// GETTER (the one the engine calls to append every declared child, static or a Repeater) forces a
// style default content item into existence on first access if none is set yet; the FIRST time a
// Repeater's own transparent-for-positioner reparenting kicks in, it can land under whatever
// content item won that race. A vanilla QtQuick.Controls Menu has the exact same quirk (verified
// empirically) — mixing a bare Repeater with static siblings in a Menu is a known rough edge, not
// something particular to this project. `insertItem(index, object)` sidesteps it entirely: we pick
// the index ourselves, from our own authored structure, independent of contentModel's incidental
// state. The running index is `<static items/separators declared before this point>` (a
// compile-time constant) `+ <sum of .count over every earlier dynamic block's Instantiator>` (so
// multiple <For>/<Show> blocks compose correctly; a trailing static item needs no adjustment — it
// keeps whatever position contentData_append already gave it, and insertItem at a lower index
// simply shifts it up, same as it would for another static item declared earlier).
//
// <For> row index: routed through the reserved `__forIndex` property (qml.ts's rowDelegate), NEVER
// the bare `index` context property — see commit 82ef306: a handler the row declares (unlikely
// here, but the rule is unconditional) could otherwise shadow it.

/** `onClick`/`disabled`/`checked` off a <MenuItem>'s props (raw nodes — resolved against the
 *  caller's scope, which differs for a static item vs. a <For> row). Checked against the attrs.ts
 *  contract itself — the generic checkElement recursion (PARENT_CONSUMED) only reaches a <MenuItem>
 *  that's a DIRECT child of <Menu>/<ContextMenu>, not one reached through a <For> row or a <Show> —
 *  so every caller here runs the same fail-loud check regardless of where the item sits. */
function itemPropsOf(propsArg: t.Node | undefined): { onClick?: t.Node; disabled?: t.Node; checked?: t.Node } {
  checkAttrs("MenuItem", propsArg);
  const props = propsOf(propsArg);
  return { onClick: props.get("onClick"), disabled: props.get("disabled"), checked: props.get("checked") };
}

/** One W.MenuItem object, `level`-indented. `checked` implies `checkable: true`; `disabled` reuses
 *  the same HTML-attribute convention as every other native control (bare or reactive). Shared by
 *  static items and <For> rows — only `scope` (and the label/level) differ. */
function renderMenuItemLines(labelBinding: string, p: { onClick?: t.Node; disabled?: t.Node; checked?: t.Node },
                              scope: Scope, level: number): string[] {
  const i = (n: number) => INDENT.repeat(level + n);
  const lines = [`${i(0)}W.MenuItem {`, `${i(1)}text: ${labelBinding}`];
  if (p.checked) {
    lines.push(`${i(1)}checkable: true`);
    lines.push(`${i(1)}checked: ${emitExpr(p.checked as t.Expression, { ...scope, mode: "binding" })}`);
  }
  if (p.disabled) {
    const d = boolAttrExpr(p.disabled, scope);
    if (d) lines.push(`${i(1)}enabled: ${enabledFromDisabled(d)}`);
  }
  if (p.onClick) lines.push(`${i(1)}onTriggered: { ${handlerBody(p.onClick, scope)} }`);
  lines.push(`${i(0)}}`);
  return lines;
}

function menuEachOf(propsArg: t.Node | undefined, scope: Scope): string {
  const each = propsOf(propsArg).get("each");
  return each ? emitExpr(each, { ...scope, mode: "binding" }) : "[]";
}

function menuWhenOf(propsArg: t.Node | undefined, scope: Scope): string {
  const when = propsOf(propsArg).get("when");
  return when ? emitExpr(when, { ...scope, mode: "binding" }) : "true";
}

/** One dynamic block's Instantiator wiring: model + delegate (the row/gated item) + the two
 *  handlers that do the ACTUAL contentModel insert/remove (the Qt-documented pattern — see the
 *  comment above). `baseIndex` is this block's running index expression (everything declared
 *  before it); `id` is this Instantiator's own id, so a LATER block can add `${id}.count` to ITS
 *  base index. */
/** `itemLines` must already be indented at `level + 2` (the slot inside `delegate: Component { }`)
 *  — callers build them with `renderMenuItemLines(…, level + 2)` / `INDENT.repeat(level + 2)`. */
function instantiatorLines(id: string, model: string, menuId: string, baseIndex: string,
                            itemLines: string[], level: number): string[] {
  const i = (n: number) => INDENT.repeat(level + n);
  return [
    `${i(0)}Instantiator {`,
    `${i(1)}id: ${id}`,
    `${i(1)}model: ${model}`,
    `${i(1)}delegate: Component {`,
    ...itemLines,
    `${i(1)}}`,
    `${i(1)}onObjectAdded: (index, object) => ${menuId}.insertItem(${baseIndex} + index, object)`,
    `${i(1)}onObjectRemoved: (index, object) => ${menuId}.removeItem(object)`,
    `${i(0)}}`,
  ];
}

/** <For each={E}>{(row[, i]) => <MenuItem>…</MenuItem>}</For> (or a <MenuSeparator> row) inside
 *  <Menu>/<ContextMenu> → an Instantiator whose delegate IS the row. `row` routes through
 *  `modelData` and `i()` through the reserved `__forIndex` property. Returns the emitted lines AND
 *  this block's Instantiator id (the caller threads it into the running base-index for whatever
 *  comes after). */
function renderForBlock(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, owner: string,
                        menuId: string, id: string, baseIndex: string): string[] {
  checkAttrs("For", propsArg);
  rejectForKey(propsArg, `<${owner}>`);
  const each = menuEachOf(propsArg, scope);
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as Fn | undefined;
  if (!delegate) throw new Error(`<${owner}><For> needs a row function: {(item) => <MenuItem>…</MenuItem>}`);
  const [param, indexParam] = delegate.params;
  const itemName = param && t.isIdentifier(param) ? param.name : null;
  const indexName = indexParam && t.isIdentifier(indexParam) ? indexParam.name : null;
  const body = delegate.body;
  if (!isHCall(body))
    throw new Error(`<${owner}><For> row must be a single <MenuItem>/<MenuSeparator>, got an expression`);
  const { tag, props: rowProps, children: rowKids } = hParts(body as t.CallExpression);
  const rowName = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : "?";
  if (rowName !== "MenuItem" && rowName !== "MenuSeparator")
    throw new Error(`<${owner}><For> row must be a single <MenuItem>/<MenuSeparator>, got <${rowName}>`);

  const rowScope: Scope = {
    ...scope,
    locals: { ...(scope.locals ?? {}), ...(itemName ? { [itemName]: "modelData" } : {}) },
    accessors: { ...(scope.accessors ?? {}), ...(indexName ? { [indexName]: "__forIndex" } : {}) },
  };

  let itemLines: string[];
  if (rowName === "MenuSeparator") {
    checkAttrs("MenuSeparator", rowProps);
    itemLines = [`${INDENT.repeat(level + 2)}W.MenuSeparator { }`];
  } else {
    const labelBinding = textBindingOf(rowKids as t.Node[], rowScope);
    itemLines = renderMenuItemLines(labelBinding, itemPropsOf(rowProps), rowScope, level + 2);
    // Declared whenever the row HAS an index param at all (not only when it's read inside a
    // further-nested loop) — same unconditional rule as qml.ts's rowDelegate.
    if (indexName) itemLines.splice(1, 0, `${INDENT.repeat(level + 3)}property int __forIndex: index`);
  }
  return instantiatorLines(id, each, menuId, baseIndex, itemLines, level);
}

/** <Show when={C}>…one or more <MenuItem>/<MenuSeparator>…</Show> inside <Menu>/<ContextMenu> →
 *  one `Instantiator { model: (C) ? 1 : 0; <item> }` per gated item, same insertItem/removeItem
 *  wiring as <For>. No fallback (menus don't need one here); nesting <For>/<Show> inside the gate
 *  is out of scope — fail loud. Returns the emitted lines AND the ids of every Instantiator it
 *  created (in order) — the caller folds each into the running base-index. */
function renderShowBlock(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, owner: string,
                         menuId: string, nextId: () => string, baseIndexOf: () => string): { lines: string[]; ids: string[] } {
  checkAttrs("Show", propsArg);
  if (propsOf(propsArg).get("fallback"))
    throw new Error(`<${owner}><Show fallback> is not supported inside a menu — the fallback branch has no item to show`);
  const when = menuWhenOf(propsArg, scope);
  const out: string[] = [];
  const ids: string[] = [];
  for (const child of children) {
    if (!isHCall(child)) continue; // whitespace between items
    const { tag, props: itemProps, children: itemKids } = hParts(child as t.CallExpression);
    const name = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : "?";
    let itemLines: string[];
    if (name === "MenuSeparator") {
      checkAttrs("MenuSeparator", itemProps);
      itemLines = [`${INDENT.repeat(level + 2)}W.MenuSeparator { }`];
    } else if (name === "MenuItem") {
      const labelBinding = textBindingOf(itemKids as t.Node[], scope);
      itemLines = renderMenuItemLines(labelBinding, itemPropsOf(itemProps), scope, level + 2);
    } else {
      throw new Error(`<${owner}><Show> only wraps <MenuItem>/<MenuSeparator>, got <${name}>`);
    }
    const id = nextId();
    // `ids` so far are THIS Show's own earlier wrapped items — not yet folded into the caller's
    // running `dynamicIds` (that happens after this function returns), so fold them in here too.
    const base = [baseIndexOf(), ...ids.map((prior) => `${prior}.count`)].join(" + ");
    out.push(...instantiatorLines(id, `(${when}) ? 1 : 0`, menuId, base, itemLines, level));
    ids.push(id);
  }
  return { lines: out, ids };
}

/** The full item body of <Menu>/<ContextMenu>: <MenuItem>/<MenuSeparator> (static, placed by Qt's
 *  normal contentData_append), <For> (one Instantiator whose delegate IS the row), <Show> (one
 *  Instantiator per wrapped item). Anything else is a clear authoring error. `menuId` is threaded
 *  into every dynamic block's insertItem/removeItem calls. */
function emitMenuItemLines(children: t.Node[], scope: Scope, owner: string, level: number, menuId: string): string[] {
  const out: string[] = [];
  // Running insertion index for the NEXT dynamic block: a compile-time count of static items/
  // separators seen so far, plus `.count` of every earlier dynamic block's Instantiator (see the
  // comment above this section for why — not contentModel's own, unreliable, incidental position).
  let staticPrefix = 0;
  const dynamicIds: string[] = [];
  const baseIndex = () => [String(staticPrefix), ...dynamicIds.map((id) => `${id}.count`)].join(" + ");
  const counter = scope.inputCounter ?? { n: 0 };
  const nextId = () => `__menuDyn${counter.n++}`;

  for (const child of children) {
    if (!isHCall(child)) continue; // whitespace between items
    const { tag, props: itemProps, children: itemKids } = hParts(child as t.CallExpression);
    const name = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : "?";
    if (name === "MenuSeparator") {
      checkAttrs("MenuSeparator", itemProps);
      out.push(`${INDENT.repeat(level)}W.MenuSeparator { }`);
      staticPrefix++;
      continue;
    }
    if (name === "MenuItem") {
      const labelBinding = textBindingOf(itemKids as t.Node[], scope);
      out.push(...renderMenuItemLines(labelBinding, itemPropsOf(itemProps), scope, level));
      staticPrefix++;
      continue;
    }
    if (name === "For") {
      const id = nextId();
      out.push(...renderForBlock(itemProps, itemKids as t.Node[], scope, level, owner, menuId, id, baseIndex()));
      dynamicIds.push(id);
      continue;
    }
    if (name === "Show") {
      const { lines, ids } = renderShowBlock(itemProps, itemKids as t.Node[], scope, level, owner, menuId, nextId, baseIndex);
      out.push(...lines);
      dynamicIds.push(...ids);
      continue;
    }
    throw new Error(`<${owner}> only accepts <MenuItem>/<MenuSeparator>/<For>/<Show> children, got <${name}>`);
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
  children: t.Node[];
  owner: string;         // "Menu" / "ContextMenu" — only for emitMenuItemLines' error messages
  classes: string[];
  title?: string;       // MenuBar submenu title (already a QML string expression)
  onCloseBody?: string;
}, scope: Scope, level: number): string[] {
  const i = (n: number) => INDENT.repeat(level + n);
  const { menuId, anchorId, children, owner, classes } = opts;
  markWidgetLib(scope);

  const authorClass = classes.map((c) => JSON.stringify(c)).join(", ");

  const lines: string[] = [
    `${i(0)}W.Menu {`,
    `${i(1)}id: ${menuId}`,
    `${i(1)}cssAncestor: ${anchorId}`,
    ...(classes.length ? [`${i(1)}authorClass: [${authorClass}]`] : []),
    ...(opts.title ? [`${i(1)}title: ${opts.title}`] : []),
    // No x/y on the menu: callers open via popup(x, y) (positions a Window popup + a fresh grab).
    ...(opts.onCloseBody ? [`${i(1)}onMenuClosed: { ${opts.onCloseBody} }`] : []),
    ...emitMenuItemLines(children, scope, owner, level + 1, menuId),
  ];

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
  const openProp = props.get("open");
  const onClose = props.get("onClose");
  const triggerProp = props.get("trigger");
  const refProp = props.get("ref");

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
    // popup(0, host.height+2) opens the menu below the trigger, SYNCHRONOUSLY in onClicked → the
    // Wayland popup grab gets a fresh input serial. Clicking again closes it (__closedAt debounce
    // swallows the dismiss click).
    const toggle = `if (${menuId}.visible) ${menuId}.close(); else if (Date.now() - ${menuId}.__closedAt > 250) ${menuId}.popup(0, ${hostId}.height + 2)`;
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
        menuId, anchorId: hostId, children, owner: "Menu", classes: classesOf(props),
        onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
      }, scope, level + 1),
      `${pad}}`,
    ];
  }

  // ── Imperative form: <Menu ref={r}> → r.open(x, y) ──────────────────────────────────────────────
  // A Popup.Window grabs input on show and the grab needs a FRESH input serial, so the menu must be
  // opened SYNCHRONOUSLY inside the triggering input handler. The author holds a ref and calls
  // r.open(sceneX, sceneY) directly in the click/handler (e.g. onContextRequested). The old reactive
  // `<Menu open={sig}>` is retired — a deferred signal loses the serial and the grab fails.
  if (refProp && t.isIdentifier(refProp)) {
    const refId = `_ref_${safeName(refProp.name)}`;
    if (scope.refs) scope.refs.push(refProp.name);
    // The host IS the popup's anchor. On Wayland the compositor positions a Popup.Window by anchoring
    // to the parentItem's boundingRect@scene (parentControlGeometry) and IGNORES the popup(x,y) offset
    // — so to open at (x,y) we MOVE this 1×1 host there (in its parent frame), then popup(0,0). A
    // zero-size host degenerates the anchor rect; 1×1 keeps it valid.
    return [
      `${pad}Item {`,
      `${i(1)}id: ${refId}`,
      `${i(1)}width: 1`,
      `${i(1)}height: 1`,
      // x/y are SCENE coords → place the anchor there (parent frame), then open below it.
      `${i(1)}function open(x, y) { var __p = ${refId}.parent.mapFromItem(null, x, y); ${refId}.x = __p.x; ${refId}.y = __p.y; ${menuId}.popup(0, 0) }`,
      `${i(1)}function close() { ${menuId}.close() }`,
      `${i(1)}Window.onActiveChanged: if (!Window.active) ${menuId}.close()`,
      ...menuObjectLines({
        menuId, anchorId: refId, children, owner: "Menu", classes: classesOf(props),
        onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
      }, scope, level + 1),
      `${pad}}`,
    ];
  }

  if (openProp) {
    throw new Error(
      "<Menu open={…}> is retired: a Wayland popup window can't be opened from a deferred reactive " +
      "signal (the input grab fails). Use <Menu ref={r}> and call r.open(x, y) SYNCHRONOUSLY inside " +
      "the click/handler that should open it.");
  }
  throw new Error("<Menu> needs trigger={<button>} (self-managed) or ref={r} (open via r.open(x, y))");
}

// ─── <MenuBar> with <Menu title="…"> children ───────────────────────────────────────────────────

/** <ContextMenu> — a right-click menu for the CONTAINING element. The host Item anchors-fills
 *  its parent box (a foreign anchored child: invisible to the CSS layout pass), and a
 *  RightButton-only MouseArea opens the popup at the cursor via T.Menu.popup(x, y) — left
 *  clicks, hover and wheel pass through untouched. Everything else (items, author class,
 *  onClose, window-deactivation close) matches <Menu>.
 *
 *  `suppressWhen={(modifiers) => boolean}` (optional, solidterm's mouse-reporting feature): an
 *  expression re-evaluated on every right-PRESS; a true result declines the press
 *  (`mouse.accepted = false`) instead of opening the menu, which lets Qt Quick's own hit-testing
 *  fall through to whatever sits BELOW this host (e.g. a TerminalView that wants to report the
 *  click to the program it's running, xterm/VTE-style) — the press (and its matching release/drag)
 *  then goes to that item directly, not through this one. `modifiers` is the raw Qt.KeyboardModifiers
 *  int off the triggering QMouseEvent. */
function emitContextMenu(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const hostId = `__menuHost${n}`;
  const menuId = `__menu${n}`;

  const props = propsOf(propsArg);
  const onClose = props.get("onClose");
  const suppressWhen = props.get("suppressWhen");
  const suppressBody = suppressWhen ? handlerBody(suppressWhen, scope, "mouse.modifiers") : undefined;
  // handlerBody yields an EXPRESSION for `(m) => expr` but STATEMENTS for `(m) => { …; return x; }`;
  // statements can't sit inside `if (…)`, so a block body becomes an immediately-invoked function.
  const blockBodied = !!suppressWhen
    && (t.isArrowFunctionExpression(suppressWhen) || t.isFunctionExpression(suppressWhen))
    && t.isBlockStatement(suppressWhen.body);
  const suppressExpr = suppressBody !== undefined && blockBodied ? `(function() { ${suppressBody} })()` : suppressBody;

  // The menu's parentItem is a 1×1 anchor MOVED to the cursor: on Wayland the compositor positions a
  // Popup.Window by anchoring to parentItem's rect and ignores the popup(x,y) offset, so a right-click
  // moves the anchor to (mouse.x, mouse.y) then popup(0,0). The outer fill Item hosts the RightButton
  // MouseArea (left clicks/hover/wheel pass through).
  const anchorId = `__ctxAnchor${n}`;
  return [
    `${pad}Item {`,
    `${i(1)}id: ${hostId}`,
    `${i(1)}anchors.fill: parent`,
    ...(guard ? [`${i(1)}visible: !!(${guard})`] : []),
    `${i(1)}MouseArea {`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}acceptedButtons: Qt.RightButton`,
    ...(suppressExpr ? [`${i(2)}onPressed: function(mouse) { if (${suppressExpr}) mouse.accepted = false }`] : []),
    `${i(2)}onClicked: function(mouse) { ${anchorId}.x = mouse.x; ${anchorId}.y = mouse.y; ${menuId}.popup(0, 0) }`,
    `${i(1)}}`,
    `${i(1)}Item {`,
    `${i(2)}id: ${anchorId}`,
    `${i(2)}width: 1`,
    `${i(2)}height: 1`,
    `${i(2)}Window.onActiveChanged: if (!Window.active) ${menuId}.close()`,
    ...menuObjectLines({
      menuId, anchorId, children, owner: "ContextMenu", classes: classesOf(props),
      onCloseBody: onClose ? handlerBody(onClose, scope) : undefined,
    }, scope, level + 2),
    `${i(1)}}`,
    `${pad}}`,
  ];
}

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
  // The QML signal is selected(item, index); the author's first param maps to `item`. The second
  // local param is named `__idx`, not `index` — a bare `index` here would shadow a <For> row's
  // own `i()` if this <ListView> sits inside one (see qml.ts rowDelegate's `indexRef` comment).
  if (selBody) lines.push(`${i(1)}onSelected: (item, __idx) => { ${selBody} }`);
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
  // Second local param named `__idx`, not `index` — same shadowing hazard as <ListView> above.
  if (selBody) lines.push(`${i(1)}onSelected: (row, __idx) => { ${selBody} }`);
  lines.push(`${pad}}`);
  return lines;
}

// ─── registration ───────────────────────────────────────────────────────────────────────────────

registerNativeTags({
  Menu: emitMenu,
  MenuBar: emitMenuBar,
  ContextMenu: emitContextMenu,
  TreeView: emitTreeView,
  ListView: emitListView,
  TableView: emitTableView,
  Tray: emitTray,
  // Only meaningful as children of <Menu>/<MenuBar>/<Tray>; a stray one is an authoring error.
  MenuItem: () => { throw new Error("<MenuItem> must be a child of <Menu> or <Tray>"); },
  MenuSeparator: () => { throw new Error("<MenuSeparator> must be a child of <Menu>"); },
});
