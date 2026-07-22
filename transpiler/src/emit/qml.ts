import * as t from "@babel/types";
import { emitExpr, type Scope } from "./expr.ts";
import { safeName } from "../names/safe.ts";
import { CONTROL_TAGS, hParts, isFragmentTag, isHCall } from "../ast/h.ts";
import { nativeTags, requireImport } from "./native/index.ts";

export const INDENT = "    ";
const TEXT_TAGS = new Set(["text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

interface Props {
  classes: string[];
  classList: Array<{ key: string; expr: t.Expression }>;
  onClick: t.Node | undefined;
  type: string | undefined; // the `type` attr (e.g. <button type="submit"> → default button)
  ref: string | undefined; // the local var name a ref={ident} binds to
  // HTML5 drag-and-drop subset: draggable + dragData on the source, onDrop on the target.
  // The drop handler's parameter receives the SOURCE's dragData (not a DragEvent).
  draggable: boolean;
  dragData: t.Expression | undefined;
  onDrop: t.Node | undefined;
}

/** Build the cssClass property line(s) for a native element.
 *  - No classes and no classList → empty (no line emitted)
 *  - Static classes only (no classList) → `cssClass: ["a", "b"]` (unchanged from before)
 *  - With classList → reactive concat expression:
 *    `cssClass: ["a"].concat(cond1 ? ["cls1"] : []).concat(cond2 ? ["cls2"] : [])`
 *  This keeps existing golden output byte-identical when classList is absent. */
export function buildCssClassLine(props: Props, scope: Scope, pad: string): string[] {
  const { classes, classList } = props;
  if (classes.length === 0 && classList.length === 0) return [];
  if (classList.length === 0) {
    // Original static form — unchanged for goldens compatibility
    return [`${pad}cssClass: [${classes.map((c) => JSON.stringify(c)).join(", ")}]`];
  }
  // Reactive form: start with static array, then chain .concat() for each classList entry
  const staticPart = `[${classes.map((c) => JSON.stringify(c)).join(", ")}]`;
  const concats = classList.map(({ key, expr }) => {
    const cond = emitExpr(expr, { ...scope, mode: "binding" });
    return `.concat(${cond} ? [${JSON.stringify(key)}] : [])`;
  }).join("");
  return [`${pad}cssClass: ${staticPart}${concats}`];
}

/** Emit QML lines for a render `h(tag, props, ...children)` call. */
export function emitQml(call: t.CallExpression, scope: Scope, level = 0, guard?: string): string[] {
  const { tag: tagArg, props: propsArg, children } = hParts(call);

  // <>…</> fragment: NO node of its own — the children emit inline into the parent
  // (true fragment semantics; a root-level fragment is wrapped by emitComponentType).
  if (isFragmentTag(tagArg)) {
    const out: string[] = [];
    for (const k of children) if (isHCall(k)) out.push(...emitQml(k, scope, level, guard));
    return out;
  }

  if (t.isIdentifier(tagArg)) {
    if (tagArg.name === "Show") return emitShow(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "For") return emitFor(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Index") return emitIndex(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Switch") return emitSwitch(children as t.Node[], scope, level, guard);
    if (tagArg.name === "Dynamic") return emitDynamic(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Suspense") return emitSuspense(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Window") return emitWindow(propsArg, children as t.Node[], scope, level);
    // Calendar is a builtin widget (Phase 5). It is dispatched BEFORE scope.components so a user
    // component named "Calendar" is shadowed by the builtin — same precedence as all other
    // builtin identifiers (control-flow tags win over user components in the import graph too).
    if (tagArg.name === "Calendar") return emitCalendar(propsArg, scope, level, guard);
    // Native-only tag registry (plan phases 2–3): registered tags shadow user components,
    // same precedence as Calendar above.
    if (nativeTags.has(tagArg.name))
      return nativeTags.get(tagArg.name)!(propsArg, children as t.Node[], scope, level, guard);
    if (CONTROL_TAGS.has(tagArg.name)) throw new Error(`control flow ${tagArg.name} not supported in this plan`);
    if (scope.components?.has(tagArg.name)) return emitInstance(tagArg.name, propsArg, children as t.Node[], scope, level, guard);
    throw new Error(`unknown component or control flow: ${tagArg.name}`);
  }
  // <X.Provider> where X is a known context → a transparent container that IS the provider
  // instance (its exposed members come from emitComponentType: signal props + fn-member props).
  if (t.isMemberExpression(tagArg) && !tagArg.computed && t.isIdentifier(tagArg.object)
      && t.isIdentifier(tagArg.property, { name: "Provider" }) && scope.contexts?.has(tagArg.object.name)) {
    return emitProvider(children as t.Node[], scope, level, guard);
  }
  if (!t.isStringLiteral(tagArg)) throw new Error("unsupported tag expression");
  const tag = tagArg.value;
  const pad = INDENT.repeat(level);
  const props = readProps(propsArg);
  const classLine = buildCssClassLine(props, scope, `${pad}${INDENT}`);

  if (tag === "button") return emitButton(props, children as t.Node[], scope, level, guard);
  if (tag === "img") return emitImage(propsArg, props, scope, level, guard);
  // Native-only registry also owns lowercase HTML mappings (<progress>, <fieldset>, <dialog>, …).
  if (nativeTags.has(tag)) return nativeTags.get(tag)!(propsArg, children as t.Node[], scope, level, guard);
  if (tag === "input") return emitInput(propsArg, props, scope, level, guard);
  if (tag === "textarea") return emitTextarea(propsArg, props, scope, level, guard);
  if (tag === "select") return emitSelect(propsArg, props, children as t.Node[], scope, level, guard);

  if (TEXT_TAGS.has(tag)) {
    // Text primitive → the cached W.Text component (compile once, reuse/AOT — see Div.qml). Omit
    // cssPrimitive for <text> (the component default); set it for span/h1-6/p/cite/bio.
    if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
    return [
      `${pad}W.Text {`,
      ...classLine,
      ...guardLine(guard, level),
      ...(tag !== "text" ? [`${pad}${INDENT}cssPrimitive: ${JSON.stringify(tag)}`] : []),
      `${pad}${INDENT}text: ${textBinding(children as t.Node[], scope)}`,
      `${pad}}`,
    ];
  }

  const refLine: string[] = [];
  if (props.ref) {
    refLine.push(`${pad}${INDENT}id: _ref_${safeName(props.ref)}`);
    if (scope.refs) scope.refs.push(props.ref);
  }
  // onClick on a plain element (the web allows it anywhere): a filling MouseArea like the
  // button's, hover-tracked the same way — an interactive element is exactly where `:hover`
  // rules land (menu items, carousel dots, list rows).
  const clickLines: string[] = [];
  const stateLine: string[] = [];
  const clickHandler = emitHandler(props.onClick, scope);
  if (clickHandler) {
    const counter = scope.hoverCounter ?? { n: 0 };
    const maId = `__hover${counter.n++}`;
    stateLine.push(`${pad}${INDENT}cssState: ${maId}.containsMouse ? ["hover"] : []`);
    clickLines.push(
      `${pad}${INDENT}MouseArea {`,
      `${pad}${INDENT}${INDENT}id: ${maId}`,
      `${pad}${INDENT}${INDENT}anchors.fill: parent`,
      `${pad}${INDENT}${INDENT}hoverEnabled: true`,
      `${pad}${INDENT}${INDENT}cursorShape: Qt.PointingHandCursor`,
      `${pad}${INDENT}${INDENT}onClicked: ${clickHandler}`,
      `${pad}${INDENT}}`,
    );
  }
  // Drag source: QtQuick's REAL item drag (Drag attached + MouseArea drag.target). The item
  // follows the pointer; on release Drag.drop() delivers to the DropArea under the cursor
  // and the parent box is re-laid-out so the card snaps home.
  if (props.draggable) {
    const counter = scope.hoverCounter ?? { n: 0 };
    const dragId = `__drag${counter.n++}`;
    const rootId = props.ref ? `_ref_${safeName(props.ref)}` : `${dragId}_root`;
    if (!props.ref) refLine.push(`${pad}${INDENT}id: ${rootId}`);
    const dataExpr = props.dragData ? emitExpr(props.dragData, { ...scope, mode: "binding" }) : "undefined";
    // Declared children (this MouseArea included) are REPARENTED into the box's content
    // holder, so `parent` is NOT the card — the drag must target the element root by id,
    // otherwise the content slides around inside a stationary shell.
    stateLine.push(
      `${pad}${INDENT}property var __dragData: ${dataExpr}`,
      `${pad}${INDENT}Drag.active: ${dragId}.drag.active`,
      `${pad}${INDENT}Drag.hotSpot.x: width / 2`,
      `${pad}${INDENT}Drag.hotSpot.y: height / 2`,
      `${pad}${INDENT}z: ${dragId}.drag.active ? 1000 : 0`,
    );
    clickLines.push(
      `${pad}${INDENT}MouseArea {`,
      `${pad}${INDENT}${INDENT}id: ${dragId}`,
      `${pad}${INDENT}${INDENT}anchors.fill: parent`,
      `${pad}${INDENT}${INDENT}drag.target: ${rootId}`,
      `${pad}${INDENT}${INDENT}cursorShape: Qt.OpenHandCursor`,
      `${pad}${INDENT}${INDENT}onReleased: { ${rootId}.Drag.drop(); if (typeof cssLayout !== "undefined") cssLayout.notifyParentLayout(${rootId}) }`,
      `${pad}${INDENT}}`,
    );
  }
  // Drop target: a filling DropArea; the handler's parameter receives the source's dragData.
  if (props.onDrop) {
    const fn = props.onDrop;
    if (!(t.isArrowFunctionExpression(fn) || t.isFunctionExpression(fn)) || t.isBlockStatement(fn.body))
      throw new Error("onDrop must be an inline expression-bodied arrow in this plan");
    const param = fn.params[0];
    const paramName = param && t.isIdentifier(param) ? param.name : null;
    const dropScope: Scope = { ...scope, mode: "handler",
      locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "drop.source.__dragData" } : {}) } };
    // A draggable card travels WITH the drop hotspot, so its OWN DropArea would receive its
    // drop (a self-reorder no-op). Disable it while this very element is the drag source.
    const dragGuard = props.draggable ? `__drag${(scope.hoverCounter?.n ?? 1) - 1}` : null;
    clickLines.push(
      `${pad}${INDENT}DropArea {`,
      `${pad}${INDENT}${INDENT}anchors.fill: parent`,
      ...(dragGuard ? [`${pad}${INDENT}${INDENT}enabled: !${dragGuard}.drag.active`] : []),
      `${pad}${INDENT}${INDENT}onDropped: (drop) => { ${emitExpr(fn.body, dropScope)} }`,
      `${pad}${INDENT}}`,
    );
  }
  // Block primitive → the cached W.Div component (compile once, reuse/AOT — see Div.qml). Omit
  // cssPrimitive for <div> (the component default); set it for section/article/etc. Interactive
  // variants keep their MouseArea/Drag/DropArea as children of the instance (stateLine/clickLines).
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  return [
    `${pad}W.Div {`,
    ...classLine,
    ...stateLine,
    ...refLine,
    ...guardLine(guard, level),
    ...(tag !== "div" ? [`${pad}${INDENT}cssPrimitive: ${JSON.stringify(tag)}`] : []),
    ...emitChildren(children as t.Node[], scope, level + 1),
    ...clickLines,
    `${pad}}`,
  ];
}

/** <Comp prop={v}>children</Comp> → Comp { prop: <v>; <children> }. Children mount into the type's
 *  inherited default property (`data`), where the component's root-level {props.children} resolves. */
function emitInstance(name: string, propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const typeName = scope.components?.get(name) ?? name;
  // A foreign `.qml` component (escape hatch) has a plain (non-Css) root, so the layout engine
  // can't flow it. Wrap the instance in a bare Css box: the engine sizes the box to the foreign
  // child's implicit size and places it, so it participates in the parent's flex/grid.
  if (scope.foreignQml?.has(typeName)) {
    const i = (n: number) => INDENT.repeat(level + n);
    // A registered-module type additionally pulls its `import <Uri> <ver> as QM_*` line into
    // this component's header (same requireImport channel the opt-in viz modules use).
    const importLine = scope.foreignImports?.get(typeName);
    if (importLine) requireImport(scope, importLine);
    // Sizing contract (owner note 2026-07-04): a QML item has NO size unless given one. So the CSS
    // box acquires its size the normal way — from the parent's flex/flow, a `class` on the tag, or
    // width/height props — and imposes it DOWN onto the foreign via `anchors.fill: parent`. All three
    // sources feed the SAME box; measuring the foreign's own implicit is only the fallback for a
    // component that sets implicitWidth/Height itself (e.g. a natural-size badge).
    const props = readProps(propsArg);
    const classLine = buildCssClassLine(props, scope, i(1));
    const inner = [`${i(1)}${typeName} {`, `${i(2)}anchors.fill: parent`];
    if (propsArg && t.isObjectExpression(propsArg)) {
      for (const p of propsArg.properties) {
        if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
        const k = p.key.name;
        if (k === "children" || k === "class" || k === "classList") continue;
        // width/height size the WRAPPER, not the item: route them to the foreign's implicit, which the
        // layout engine reads to size the box (the box then fills the item back via anchors.fill).
        const key = k === "width" ? "implicitWidth" : k === "height" ? "implicitHeight" : safeName(k);
        inner.push(`${i(2)}${key}: ${emitExpr(p.value, { ...scope, mode: "binding" })}`);
      }
    }
    inner.push(...emitChildren(children, scope, level + 2));
    inner.push(`${i(1)}}`);
    return [`${pad}Css.CssRect {`, `${i(1)}cssPrimitive: "div"`, ...classLine, ...guardLine(guard, level), ...inner, `${pad}}`];
  }
  const meta = scope.componentMeta?.get(name);
  const lines = [`${pad}${typeName} {`, ...guardLine(guard, level)];

  // Provider instance: give it a stable id and expose it on the provider stack for its children, so
  // any consumer of this ctx mounted within receives `__ctx_<ctx>: <id>`.
  let childScope = scope;
  if (meta?.provides) {
    if (scope.providerStack?.[meta.provides] !== undefined)
      throw new Error(`nested <${meta.provides}.Provider> of the same context is not supported in this plan`);
    const counter = scope.ctxProvCounter ?? { n: 0 };
    const id = `__ctxprov${counter.n++}`;
    lines.push(`${pad}${INDENT}id: ${id}`);
    childScope = { ...scope, ctxProvCounter: counter, providerStack: { ...(scope.providerStack ?? {}), [meta.provides]: id } };
  }

  // Consumer instance: inject the provider instance for each consumed ctx that is on the stack.
  if (meta?.consumes) {
    for (const ctx of meta.consumes) {
      const id = scope.providerStack?.[ctx];
      if (id) lines.push(`${pad}${INDENT}__ctx_${safeName(ctx)}: ${id}`);
      else throw new Error(`consumer ${name} uses context ${ctx} with no provider in scope`);
    }
  }

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key) && p.key.name !== "children" && t.isExpression(p.value))
        lines.push(`${pad}${INDENT}${safeName(p.key.name)}: ${emitExpr(p.value, { ...scope, mode: "binding" })}`);
    }
  }
  lines.push(...emitChildren(children, childScope, level + 1));
  lines.push(`${pad}}`);
  return lines;
}

/** <Ctx.Provider value={V}>children</Ctx.Provider> → a transparent passthrough container (a "div"
 *  that fills its parent) that slots the children. The container is the provider component's render
 *  root; emitComponentType decorates it with the value members (signals already; fn members as
 *  `property var <name>`), so the provider INSTANCE carries the context value. */
function emitProvider(children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  return [
    `${pad}Css.CssRect {`,
    ...guardLine(guard, level),
    `${pad}${INDENT}cssPrimitive: "div"`,
    ...emitChildren(children, scope, level + 1),
    `${pad}}`,
  ];
}

export function guardLine(guard: string | undefined, level: number): string[] {
  return guard ? [`${INDENT.repeat(level)}${INDENT}visible: !!(${guard})`] : [];
}

function invertedGuardLine(when: string, outerGuard: string | undefined, level: number): string[] {
  const expr = outerGuard ? `(${outerGuard}) && !(${when})` : `!(${when})`;
  return [`${INDENT.repeat(level)}${INDENT}visible: ${expr}`];
}

/** <Window title width height visible>children</Window> → a real QML Window whose painted surface
 *  is a `qml-window` CssRect filling it (the stylesheet drives layout). The reserved prop names
 *  (width/height/visible/title) map to the REAL QML Window properties — NOT safeName-escaped. The
 *  Window is the app root; emitComponentType sees its `id: __self` and won't add a second id. */
function emitWindow(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const w = readWindowProps(propsArg, scope);
  const lines = [`${pad}Window {`, `${i(1)}id: __self`, `${i(1)}visible: ${w.visible}`];
  if (w.width !== null) lines.push(`${i(1)}width: ${w.width}`);
  if (w.height !== null) lines.push(`${i(1)}height: ${w.height}`);
  if (w.title !== null) lines.push(`${i(1)}title: ${w.title}`);
  lines.push(
    // Ctrl+Tab / Ctrl+Shift+Tab walk the same focus chain as Tab. Qt Quick's own tab
    // handling skips key events carrying Control/Alt (qquickitem.cpp deliverKeyEvent),
    // so without these shortcuts Ctrl+Tab is dead — and it's the desktop way OUT of a
    // textarea, where plain Tab types a tab character.
    `${i(1)}Shortcut {`,
    `${i(2)}sequences: ["Ctrl+Tab"]`,
    `${i(2)}enabled: solidTabstop.enabled`,
    `${i(2)}onActivated: { var __it = __self.activeFocusItem || __self.contentItem; var __nx = __it.nextItemInFocusChain(true); if (__nx) __nx.forceActiveFocus(Qt.TabFocusReason) }`,
    `${i(1)}}`,
    `${i(1)}Shortcut {`,
    `${i(2)}sequences: ["Ctrl+Shift+Tab", "Ctrl+Backtab"]`,
    `${i(2)}enabled: solidTabstop.enabled`,
    `${i(2)}onActivated: { var __it = __self.activeFocusItem || __self.contentItem; var __nx = __it.nextItemInFocusChain(false); if (__nx) __nx.forceActiveFocus(Qt.BacktabFocusReason) }`,
    `${i(1)}}`,
    `${i(1)}Css.CssRect {`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}cssClass: ["qml-window"]`,
    `${i(2)}cssPrimitive: "window"`,
    ...emitChildren(children, scope, level + 2),
    `${i(1)}}`,
    // Global keyboard tab-focus marker (study §6): W.Tabstop follows the window's activeFocusItem and
    // frames whatever holds keyboard focus — universal, no per-widget CSS. Styled via the `::tab-stop`
    // pseudo-element (a decoration, like `::before`; distinct from `:focus`, the element's own state).
    `${i(1)}W.Tabstop { window: __self }`,
    // Tab-stop is born by default: on load, put keyboard focus on the first focusable so Tab works
    // immediately and the ring is visible without a click first (study §6; honors the opt-out).
    `${i(1)}Component.onCompleted: if (solidTabstop.enabled) Qt.callLater(function() { var f = __self.contentItem.nextItemInFocusChain(true); if (f) f.forceActiveFocus(Qt.TabFocusReason) })`,
    `${pad}}`,
  );
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  return lines;
}

/** Read a <Window>'s reserved props to QML literals (visible defaults true; `visible` shorthand → true). */
function readWindowProps(propsArg: t.Node | undefined, scope: Scope): { visible: string; width: string | null; height: string | null; title: string | null } {
  const out = { visible: "true", width: null as string | null, height: null as string | null, title: null as string | null };
  if (!propsArg || !t.isObjectExpression(propsArg)) return out;
  const bind: Scope = { ...scope, mode: "binding" };
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
    if (p.key.name === "visible") out.visible = (t.isBooleanLiteral(p.value) ? p.value.value : true) ? "true" : "false";
    else if (p.key.name === "width") out.width = emitExpr(p.value, bind);
    else if (p.key.name === "height") out.height = emitExpr(p.value, bind);
    else if (p.key.name === "title") out.title = emitExpr(p.value, bind);
  }
  return out;
}

function emitButton(props: Props, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const isElement = (c: t.Node) => isHCall(c);
  const textKids = children.filter((c) => !isElement(c));
  const elemKids = children.filter(isElement);
  // One .qml per component (owner directive): instantiate the widget-library CssButton and wire
  // props/children — the button's internals (hover state, label, MouseArea) live in the .qml, not
  // here. The `import "widgets" as W` header is added by emitComponentType when widgetLib is set.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  // <button type="submit"> is the dialog's default button (owner call 2026-07-06): HTML's submit
  // button is the Enter-activated one, so the enclosing Dialog fires it on Enter. `isDefault` also
  // surfaces as a `default` cssState, so `button:default { … }` can emphasise it.
  const isSubmit = props.type === "submit";
  const lines = [
    `${pad}W.Button {`,
    ...classLine,
    ...guardLine(guard, level),
    ...(isSubmit ? [`${i(1)}isDefault: true`] : []),
    `${i(1)}text: ${textBinding(textKids, scope)}`,
    ...emitChildren(elemKids, scope, level + 1),
  ];
  const handler = emitHandler(props.onClick, scope);
  if (handler) lines.push(`${i(1)}onClicked: ${handler}`);
  lines.push(`${pad}}`);
  return lines;
}

/** <img class="a" src={u} /> → W.Image (CssImage does object-fit + rounded-rect clip via MultiEffect,
 *  so `border-radius` yields a real circular avatar). One .qml per component: the CssImage + `src`
 *  slot live in Image.qml; the emit just wires the class and source. */
function emitImage(propsArg: t.Node | undefined, props: Props, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  // Resolve the `src` prop via emitExpr in binding mode.
  let src = '""';
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "src" }) && t.isExpression(p.value))
        src = emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  return [
    `${pad}W.Image {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}src: ${src} || ""`,
    `${pad}}`,
  ];
}

/** Translate an onInput/onKeyDown arrow handler body to QML-JS, mapping the event param and DOM idioms.
 *  - `e.currentTarget.value` / `e.target.value` → `text` (the control's text property in scope)
 *  - `e.key === "Enter"` → `(event.key === Qt.Key_Return || event.key === Qt.Key_Enter)`
 *  - `e.key === "Escape"` → `(event.key === Qt.Key_Escape)`
 *  - `e.key` → `event.key`
 *  Emits the body via emitExpr (with the event param aliased to "__ev"), then post-processes
 *  the string to replace DOM patterns with their QML equivalents. */
function translateInputHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  let body: string;
  if (t.isBlockStatement(fn.body)) {
    // Block-bodied handler: emit each statement, join with space
    body = fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  } else {
    body = emitExpr(fn.body, inner);
  }
  return body
    .replace(/__ev\.(?:currentTarget|target)\.value/g, "text")
    .replace(/__ev\.key\s*===\s*"Enter"/g, "(event.key === Qt.Key_Return || event.key === Qt.Key_Enter)")
    .replace(/__ev\.key\s*===\s*"Escape"/g, "(event.key === Qt.Key_Escape)")
    .replace(/__ev\.key/g, "event.key");
}

/** Parse all widget-relevant attrs from a propsArg ObjectExpression into a plain record.
 *  Returns the typed, name-normalised set of values the widget emitters need. */
function readWidgetProps(propsArg: t.Node | undefined, scope: Scope): {
  type: string; valueExpr: string | null; signalName: string | null;
  placeholder: string;
  onInputFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  onKeyDownFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  disabled: boolean; readOnly: boolean; maxLength: string | null;
  // Phase 3: toggle/radio attrs
  role: string; name: string | null; checkedExpr: string | null;
  // Phase 4: range/number attrs
  min: string; max: string; step: string;
} {
  let type = "text";
  let valueExpr: string | null = null;
  let signalName: string | null = null;
  let placeholder = "";
  let onInputFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let onKeyDownFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let disabled = false;
  let readOnly = false;
  let maxLength: string | null = null;
  let role = "";
  let name: string | null = null;
  let checkedExpr: string | null = null;
  // Phase 4: range (Slider) and number (SpinBox) attrs; defaults match HTML spec.
  let min = "0";
  let max = "100";
  let step = "1";

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "type" && t.isStringLiteral(p.value)) type = p.value.value;
      if (key === "value" && t.isExpression(p.value)) {
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
        // Detect value={sig()} — zero-arg call to a signal accessor — to name the Binding's value.
        if (t.isCallExpression(p.value) && t.isIdentifier(p.value.callee) && p.value.arguments.length === 0) {
          const sym = scope.table.get(p.value.callee.name);
          if (sym?.kind === "signal") signalName = sym.name;
        }
      }
      // `checked={sig()}` — boolean controlled value for checkboxes, radios, switches.
      if (key === "checked" && t.isExpression(p.value))
        checkedExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (key === "placeholder" && t.isStringLiteral(p.value)) placeholder = p.value.value;
      if (key === "onInput" && t.isExpression(p.value)
          && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onInputFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "onChange" && t.isExpression(p.value)
          && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if ((key === "onKeyDown" || key === "onKeydown") && t.isExpression(p.value)
          && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onKeyDownFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      // `disabled` / `readonly` / `readOnly` / `maxlength` / `maxLength` — HTML attribute names.
      if (key === "disabled") disabled = !t.isBooleanLiteral(p.value) || p.value.value;
      if (key === "readonly" || key === "readOnly") readOnly = !t.isBooleanLiteral(p.value) || p.value.value;
      if (key === "maxlength" || key === "maxLength") {
        if (t.isNumericLiteral(p.value)) maxLength = String(p.value.value);
        else if (t.isExpression(p.value)) maxLength = emitExpr(p.value, { ...scope, mode: "binding" });
      }
      // `role` — "switch" on a checkbox promotes T.CheckBox to T.Switch.
      if (key === "role" && t.isStringLiteral(p.value)) role = p.value.value;
      // `name` — radio group name; radios sharing a name join the same T.ButtonGroup.
      if (key === "name" && t.isStringLiteral(p.value)) name = p.value.value;
      // `min` / `max` / `step` — numeric range for T.Slider and T.SpinBox.
      if ((key === "min" || key === "max" || key === "step") && t.isExpression(p.value)) {
        const numStr = t.isNumericLiteral(p.value) ? String(p.value.value)
          : t.isStringLiteral(p.value) ? p.value.value
          : emitExpr(p.value, { ...scope, mode: "binding" });
        if (key === "min") min = numStr;
        else if (key === "max") max = numStr;
        else step = numStr;
      }
    }
  }
  return { type, valueExpr, signalName, placeholder, onInputFn, onChangeFn, onKeyDownFn, disabled, readOnly, maxLength, role, name, checkedExpr, min, max, step };
}

/** <input type="text|password|email|search" .../>
 *  → wrapper Css.CssFill (cssPrimitive "input") carrying CSS identity + `:focus`/`:disabled` state,
 *    + inner T.TextField (background null, anchors.fill) with CSS-bridged colour/font.
 *
 *  Controlled value: a `Binding` element on the CssFill re-asserts the signal value into the
 *  control without being destroyed on user edits (unlike a plain property binding).
 *  Two-way: the author's onInput → onTextEdited handler writes back into the signal.
 *  Placeholder: a plain QML Text overlay inside T.TextField (T.TextField's own contentItem
 *  renders placeholders, but only through an attached style — we don't load one). */
function emitInput(propsArg: t.Node | undefined, props: Props, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));

  // Unique id for this control; shared counter keeps ids monotonically unique across the component.
  // The id is kept on the W.<Name> instance so the controlled Binding (target __inputN) and any
  // handler value-reads resolve against the component's two-way aliases across the boundary.
  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;

  const wp = readWidgetProps(propsArg, scope);
  const { type, role } = wp;

  // Phase 3: dispatch checkbox/switch/radio to dedicated emitters before the T.TextField path.
  if (type === "checkbox" && role === "switch") return emitSwitchToggle(props, scope, level, guard, ctlId, wp);
  if (type === "checkbox") return emitCheckboxToggle(props, scope, level, guard, ctlId, wp);
  if (type === "radio") return emitRadioButton(props, scope, level, guard, ctlId, wp);
  // Phase 4: range → T.Slider; number → T.SpinBox.
  if (type === "range") return emitSlider(props, scope, level, guard, ctlId, wp);
  if (type === "number") return emitSpinBox(props, scope, level, guard, ctlId, wp);
  // Phase 5: date → readOnly text field + MonthGrid popup.
  if (type === "date") return emitDateInput(propsArg, props, scope, level, guard, ctlId);

  const { valueExpr, signalName, placeholder, onInputFn, onChangeFn, onKeyDownFn, disabled, readOnly, maxLength } = wp;

  // One .qml per component: instantiate W.TextField (the T.TextField + placeholder + CSS-bridged
  // colour/font live in TextField.qml). Keep the id so the controlled Binding resolves.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // Handler bodies — translateInputHandler maps e.target.value → `text`, e.key → event.key, etc.
  // `text` on the instance resolves via the component's two-way `text` alias.
  const textEditedBody = onInputFn
    ? translateInputHandler(onInputFn, scope)
    : signalName ? `${safeName(signalName)} = text` : "";
  const editingFinishedBody = onChangeFn ? translateInputHandler(onChangeFn, scope) : "";
  const keyBody = onKeyDownFn ? translateInputHandler(onKeyDownFn, scope) : "";

  const lines: string[] = [
    `${pad}W.TextField {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];

  if (type === "password") lines.push(`${i(1)}echoMode: TextInput.Password`);
  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (readOnly) lines.push(`${i(1)}readOnly: true`);
  if (maxLength !== null) lines.push(`${i(1)}maximumLength: ${maxLength}`);
  if (placeholder) lines.push(`${i(1)}placeholder: ${JSON.stringify(placeholder)}`);
  if (textEditedBody) lines.push(`${i(1)}onTextEdited: { ${textEditedBody} }`);
  if (editingFinishedBody) lines.push(`${i(1)}onEditingFinished: { ${editingFinishedBody} }`);
  if (keyBody) lines.push(`${i(1)}onKeyPressed: (event) => { ${keyBody} }`);

  // Binding element: persistently asserts the signal value into the control text. Unlike a plain
  // property binding (`text: sig`), a Binding object survives user edits — the signal value is
  // re-pushed only when `value` changes (i.e., when the signal emits). restoreMode: RestoreNone
  // prevents the Binding from reverting the field when it becomes inactive.
  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "text"`,
      `${i(2)}value: ${valueExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** <textarea .../>
 *  → wrapper Css.CssFill (cssPrimitive "textarea") + T.TextArea (background null, wrapMode Wrap).
 *
 *  T.TextArea lacks `textEdited` (it extends TextEdit, not TextField/TextInput), so `onInput` and
 *  `onChange` both map to `onTextChanged`. The Binding's no-op re-assertion prevents the echo from
 *  causing visible re-renders (QML only emits textChanged when the text actually changes). */
function emitTextarea(propsArg: t.Node | undefined, props: Props, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;

  // One .qml per component: instantiate W.TextArea (the T.TextArea + wrap + placeholder + CSS-bridged
  // colour/font live in TextArea.qml). Keep the id so the controlled Binding resolves.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const { valueExpr, signalName, placeholder, onInputFn, onChangeFn, disabled, readOnly }
    = readWidgetProps(propsArg, scope);

  // T.TextArea has no textEdited signal: map both onInput and onChange to the instance's
  // onTextChanged (text is a two-way alias, so textChanged exists on the component). onInput wins.
  const fn = onInputFn ?? onChangeFn;
  const textChangedBody = fn
    ? translateInputHandler(fn, scope)
    : signalName ? `${safeName(signalName)} = text` : "";

  const lines: string[] = [
    `${pad}W.TextArea {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (readOnly) lines.push(`${i(1)}readOnly: true`);
  if (placeholder) lines.push(`${i(1)}placeholder: ${JSON.stringify(placeholder)}`);
  if (textChangedBody) lines.push(`${i(1)}onTextChanged: { ${textChangedBody} }`);

  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "text"`,
      `${i(2)}value: ${valueExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** Translate an onChange arrow handler for toggle controls (checkbox, switch, radio).
 *  Like translateInputHandler but maps e.target.checked / e.target.value → <ctlId>.checked
 *  instead of the text field's `text`. Used by onToggled handlers in all three toggle types. */
function translateToggleHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, ctlId: string, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  let body: string;
  if (t.isBlockStatement(fn.body)) {
    body = fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  } else {
    body = emitExpr(fn.body, inner);
  }
  // Map the browser-DOM patterns that toggle handlers typically use.
  return body
    .replace(/__ev\.(?:currentTarget|target)\.checked/g, `${ctlId}.checked`)
    .replace(/__ev\.(?:currentTarget|target)\.value/g, `${ctlId}.checked`);
}

/** <input type="checkbox"> → wrapper CssFill + T.CheckBox.
 *  The indicator slot hosts a Css.CssFill (20×20) with a centred glyph (✓) visible when checked.
 *  Indicator is NOT inside a Css container, so CSS geometry does NOT apply to it — width/height
 *  are hardcoded at 20px (glyph colour/background ARE CSS-styleable via .indicator rules).
 *  cssState on both wrapper and indicator carries "checked"/"disabled" for author pseudo-classes. */
function emitCheckboxToggle(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { checkedExpr, onChangeFn, disabled } = wp;
  // One .qml per component: instantiate W.Checkbox (the T.CheckBox + indicator glyph host live in
  // Checkbox.qml). Keep the id so the controlled Binding resolves.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const lines: string[] = [
    `${pad}W.Checkbox {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  // translateToggleHandler maps e.target.checked → `${ctlId}.checked`, which resolves via the
  // component's two-way `checked` alias on the instance.
  if (onChangeFn) lines.push(`${i(1)}onToggled: { ${translateToggleHandler(onChangeFn, ctlId, scope)} }`);

  if (checkedExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "checked"`,
      `${i(2)}value: ${checkedExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** <input type="checkbox" role="switch"> → wrapper CssFill + T.Switch.
 *  The indicator slot hosts a track CssFill (36×20) containing a knob CssRect (16×16) whose
 *  x position is animated by T.Switch.visualPosition (0→1). The Behavior on x gives a 120 ms
 *  slide without requiring a CSS transition (CSS animations are not yet wired). */
function emitSwitchToggle(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { checkedExpr, onChangeFn, disabled } = wp;
  // One .qml per component: instantiate W.Toggle (the T.Switch + track/knob host live in Toggle.qml).
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const lines: string[] = [
    `${pad}W.Toggle {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (onChangeFn) lines.push(`${i(1)}onToggled: { ${translateToggleHandler(onChangeFn, ctlId, scope)} }`);

  if (checkedExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "checked"`,
      `${i(2)}value: ${checkedExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** <input type="radio" name="g"> → wrapper CssFill + T.RadioButton.
 *  Radios sharing the same `name` join one T.ButtonGroup (emitted at the component root by
 *  emitComponentType after reading scope.buttonGroups). The attached property
 *  T.ButtonGroup.group on the T.RadioButton wires up exclusivity at the Qt level.
 *  Indicator (20×20): a CssFill with inner dot (CssRect, 8×8) visible only when checked;
 *  border-radius via CSS makes both circular. Geometry is hardcoded (not in a Css container). */
function emitRadioButton(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { checkedExpr, onChangeFn, disabled, name } = wp;
  if (scope.usedWidgets) scope.usedWidgets.flag = true;
  const radioState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(${ctlId}.checked ? ["checked"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  // Register this group name so emitComponentType can emit T.ButtonGroup { id: __group_<name> }.
  if (name && scope.buttonGroups) {
    if (!scope.buttonGroups.has(name)) scope.buttonGroups.set(name, []);
    scope.buttonGroups.get(name)!.push(ctlId);
  }
  const groupId = name ? `__group_${safeName(name)}` : null;

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${radioState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.RadioButton {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    `${i(2)}contentItem: null`,
    // A radio GROUP is ONE tab stop (HTML/desktop): Tab enters at the checked radio — or
    // the first, when none is checked — and Tab leaves the group; arrows move within.
    groupId
      ? `${i(2)}activeFocusOnTab: solidTabstop.enabled && (${ctlId}.checked || (!${groupId}.checkedButton && ${groupId}.buttons.length > 0 && ${groupId}.buttons[0] === ${ctlId}))`
      : `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
  ];

  // Attach to the group if a name was given; the group is declared at root level by emitComponentType.
  if (groupId) lines.push(`${i(2)}T.ButtonGroup.group: ${groupId}`);

  // Arrow keys move FOCUS through the group in DECLARATION order (the group's `order`
  // list — ButtonGroup.buttons follows attachment order, which incubation scrambles),
  // wrapping. Desktop model (tab-focus study §6): arrow-navigating a radio group MOVES THE
  // SELECTION as it moves focus — there is no focused-but-unchecked radio during arrow nav — so
  // __step CHECKS the target (exclusive group unchecks the rest) and focuses it. Space also checks
  // the focused radio (AbstractButton native). Tab leaves the group (activeFocusOnTab above).
  if (groupId) lines.push(
    `${i(2)}function __step(d) { var bs = ${groupId}.order; var j = (bs.indexOf(${ctlId}) + d + bs.length) % bs.length; bs[j].checked = true; bs[j].forceActiveFocus(Qt.TabFocusReason) }`,
    `${i(2)}Keys.onDownPressed: __step(1)`,
    `${i(2)}Keys.onRightPressed: __step(1)`,
    `${i(2)}Keys.onUpPressed: __step(-1)`,
    `${i(2)}Keys.onLeftPressed: __step(-1)`,
  );

  lines.push(
    `${i(2)}indicator: Css.CssFill {`,
    `${i(3)}cssPrimitive: "span"`,
    `${i(3)}cssClass: ["indicator"]`,
    `${i(3)}cssState: ${radioState}`,
    `${i(3)}width: 20`,
    `${i(3)}height: 20`,
    `${i(3)}implicitWidth: 20`,
    `${i(3)}implicitHeight: 20`,
    // Anchored Item host insulates the dot from the CSS flex pass (see the checkbox glyph
    // note). The nested CssItem injects background-color/radius from the .indicator-dot rule.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Rectangle {`,
    `${i(5)}visible: ${ctlId}.checked`,
    `${i(5)}anchors.centerIn: parent`,
    `${i(5)}width: 8`,
    `${i(5)}height: 8`,
    `${i(5)}radius: 4`,
    `${i(5)}color: "#2b2b2b"`,
    // State rides the CssItem's OWN class (`.indicator-dot.checked`): an ancestor-state
    // rule (`.indicator:checked .indicator-dot`) resolves at mount only — the engine's
    // ancestor-restyle pass does not reach CssItems hosted under plain items (engine gap,
    // queued). A class binding re-resolves the CssItem itself on every toggle.
    `${i(5)}Css.CssItem { cssPrimitive: "rect"; cssClass: ${ctlId}.checked ? ["indicator-dot", "checked"] : ["indicator-dot"] }`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
  );

  if (disabled) lines.push(`${i(2)}enabled: false`);
  // onChange fires when this radio BECOMES checked — by click, Space, OR arrow-nav (§6: arrows move
  // the selection). Arrow-nav sets `checked` programmatically, and `toggled()` is emitted ONLY on
  // interactive toggles (verified in qquickabstractbutton.cpp), so onToggled would miss it. Guard on
  // `checked` so the auto-UNchecked sibling in the exclusive group doesn't fire the handler, and so a
  // controlled Binding re-assert (same value) is a harmless no-op — no echo.
  if (onChangeFn) lines.push(`${i(2)}onCheckedChanged: { if (${ctlId}.checked) { ${translateToggleHandler(onChangeFn, ctlId, scope)} } }`);

  lines.push(`${i(1)}}`);

  if (checkedExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "checked"`,
      `${i(2)}value: ${checkedExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** Translate a handler that receives a synthetic event and reads `e.target.value`, mapping
 *  that accessor to `valueExpr`. Works for onMoved (Slider), onValueModified (SpinBox), and
 *  onActivated (ComboBox) where the per-event value is expressed as a QML expression string. */
function translateValueHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, valueExpr: string, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  let body: string;
  if (t.isBlockStatement(fn.body)) {
    body = fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  } else {
    body = emitExpr(fn.body, inner);
  }
  // Replace the DOM-style accessor with the QML equivalent expression.
  return body.replace(/__ev\.(?:currentTarget|target)\.value/g, valueExpr);
}

/** <input type="range" min=… max=… step=… value={} onInput={} onChange={} />
 *  → wrapper Css.CssFill (cssPrimitive "input") + T.Slider (background: track CssFill containing
 *    progress fill; handle: CssRect; from/to/stepSize from attrs).
 *
 *  Track shape: T.Slider.background is a Css.CssFill (cssClass ["track"]), 6 px tall, centred
 *  vertically within the control using Qt Basic-style x/y/width bindings (same as Qt's own Basic
 *  style). The fill CssRect inside the track shows the covered portion. The handle is a CssRect
 *  (18×18) positioned by T.Slider.visualPosition — no Behavior, dragging must be 1:1 (unlike the
 *  switch knob which only snaps at toggle time).
 *
 *  onChange deviation: HTML fires `change` on mouse-up (committed value); QML T.Slider only has
 *  `moved` (fires on every positional change) and `valueChanged` (fires on any value write including
 *  Binding re-assertions). Both onInput and onChange are mapped to onMoved — this approximates the
 *  HTML semantics close enough for reactive signal wiring; precise mouse-up semantics would need
 *  onPressedChanged and are deferred. */
function emitSlider(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { valueExpr, onInputFn, onChangeFn, disabled, min, max, step } = wp;
  // One .qml per component: instantiate W.Slider (the T.Slider + track/handle/wheel live in
  // Slider.qml). Keep the id so the controlled Binding resolves.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // Both onInput and onChange map to onMoved (see jsdoc for the approximation note). `${ctlId}.value`
  // resolves via the component's two-way `value` alias on the instance.
  const activeFn = onInputFn ?? onChangeFn;
  const onMovedBody = activeFn
    ? translateValueHandler(activeFn, `${ctlId}.value`, scope)
    : "";

  const lines: string[] = [
    `${pad}W.Slider {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}from: ${min}`,
    `${i(1)}to: ${max}`,
    `${i(1)}stepSize: ${step}`,
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (onMovedBody) lines.push(`${i(1)}onMoved: { ${onMovedBody} }`);

  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "value"`,
      `${i(2)}value: ${valueExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** <input type="number" min=… max=… step=… value={} onChange={} />
 *  → wrapper Css.CssFill (cssPrimitive "input") + T.SpinBox (editable; background null;
 *    custom contentItem TextInput; up/down indicators as Css.CssFill pairs).
 *
 *  Color/font inside contentItem: TextInput cannot read from `parent.inheritedColor` (parent
 *  is T.SpinBox, not the CssFill wrapper). Resolved via `${ctlId}.parent.inheritedColor` where
 *  `${ctlId}.parent` is the CssFill wrapper — a valid QML chain since the SpinBox IS a direct
 *  visual child of the wrapper.
 *
 *  up/down indicators: Css.CssFill positioned at the right edge of the SpinBox (x: parent.width -
 *  width). The "+" and "−" glyphs are CssText children centred inside with anchors.centerIn.
 *  Their cssState carries "active" when pressed so CSS can colour the pressed state.
 *
 *  onChange → onValueModified (T.SpinBox's signal for user-initiated value changes; excludes
 *  Binding re-assertions, so no echo loop). The synthetic event exposes `.target.value`. */
function emitSpinBox(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { valueExpr, onChangeFn, disabled, min, max, step } = wp;
  // One .qml per component: instantiate W.SpinBox (the T.SpinBox + TextInput + +/- indicators live in
  // SpinBox.qml). Keep the id so the controlled Binding resolves.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // `${ctlId}.value` resolves via the component's two-way `value` alias on the instance.
  const onValueModifiedBody = onChangeFn
    ? translateValueHandler(onChangeFn, `${ctlId}.value`, scope)
    : "";

  const lines: string[] = [
    `${pad}W.SpinBox {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}from: ${min}`,
    `${i(1)}to: ${max}`,
    `${i(1)}stepSize: ${step}`,
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (onValueModifiedBody) lines.push(`${i(1)}onValueModified: { ${onValueModifiedBody} }`);

  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "value"`,
      `${i(2)}value: ${valueExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

/** Static option entry parsed from a <option value="v">Label</option> child of <select>. */
interface OptionEntry { label: string; value: string }

/** Parse the <option> children of a <select> into a list of {label, value} pairs.
 *  Throws a clear transpiler error on dynamic content (interpolations, sub-elements, computed values). */
function parseOptions(children: t.Node[]): OptionEntry[] {
  const entries: OptionEntry[] = [];
  for (const child of children) {
    if (!isHCall(child)) continue; // skip whitespace JSXText between options
    const { tag, props: optProps, children: optChildren } = hParts(child as t.CallExpression);
    if (!t.isStringLiteral(tag) || tag.value !== "option")
      throw new Error(`<select> only accepts <option> children, got <${t.isStringLiteral(tag) ? tag.value : "?"}>`);
    // Collect label text (static strings only).
    let label = "";
    for (const c of optChildren) {
      if (t.isStringLiteral(c)) { label += c.value; }
      else if (isJsxText(c)) { label += (c as any).value; }
      else throw new Error("dynamic <option> not supported yet — use static string children only");
    }
    label = label.trim();
    // Collect value prop (defaults to label when absent).
    let value = label;
    if (optProps && t.isObjectExpression(optProps)) {
      for (const p of (optProps as t.ObjectExpression).properties) {
        if (!t.isObjectProperty(p) || !t.isIdentifier(p.key, { name: "value" })) continue;
        if (t.isStringLiteral(p.value)) { value = p.value.value; }
        else if (t.isNumericLiteral(p.value)) { value = String(p.value.value); }
        else throw new Error("dynamic <option> value not supported yet — use static string/number values only");
      }
    }
    entries.push({ label, value });
  }
  return entries;
}

/** <select value={} onChange={}><option value="v">Label</option>…</select>
 *  → wrapper Css.CssFill (cssPrimitive "select") + T.ComboBox (model from static options).
 *
 *  Values: a `readonly property var __values: [...]` on the T.ComboBox holds the parallel values
 *  array; the model holds the display labels. Controlled binding: a Binding on `currentIndex`
 *  computes indexOf(valueExpr) into the values array.
 *
 *  Visual structure:
 *  - contentItem: Css.CssText (cssClass ["value"]) showing displayText; leftPadding: 12 clears the border.
 *  - Chevron: Css.CssText (cssClass ["chevron"]) as a direct T.ComboBox child, anchored right-center.
 *  - delegate: T.ItemDelegate per row; background CssFill (cssClass ["option"], cssState "hover" when
 *    highlighted, "selected" when this row's index == currentIndex); contentItem CssText (["option-label"]).
 *  - popup: T.Popup (padding: 1 per G3 — prevents popup CssFill border clipping); background CssFill
 *    (["popup"]); contentItem ListView (clip, cap 240 px, model from delegateModel).
 *
 *  onChange → onActivated (index) with synthetic value __input<n>.__values[index].
 *  Dynamic <option> children (interpolations or <For>) throw a clear transpiler error. */
function emitSelect(propsArg: t.Node | undefined, props: Props, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  const idx = counter.n++;
  const ctlId = `__input${idx}`;

  // One .qml per component: instantiate W.Select (the T.ComboBox + delegate + Item-popup live in
  // Select.qml). Keep the id so the controlled Binding and onActivated value-reads resolve.
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // Parse static <option> children.
  const options = parseOptions(children);
  const modelArr = `[${options.map((o) => JSON.stringify(o.label)).join(", ")}]`;
  const valuesArr = `[${options.map((o) => JSON.stringify(o.value)).join(", ")}]`;

  // Read value and onChange from the <select> propsArg directly.
  let valueExpr: string | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let disabled = false;
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value))
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (key === "onChange" && t.isExpression(p.value)
          && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "disabled") disabled = !t.isBooleanLiteral(p.value) || p.value.value;
    }
  }

  // onActivated(index): translate onChange so e.target.value → the picked value. `${ctlId}.values`
  // resolves against the component's `values` array on the instance.
  const onActivatedBody = onChangeFn
    ? translateValueHandler(onChangeFn, `${ctlId}.values[index]`, scope)
    : "";

  const lines: string[] = [
    `${pad}W.Select {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}model: ${modelArr}`,
    `${i(1)}values: ${valuesArr}`,
  ];

  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (onActivatedBody) lines.push(`${i(1)}onActivated: (index) => { ${onActivatedBody} }`);

  // Binding: keep currentIndex in sync with the controlled value expression.
  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "currentIndex"`,
      `${i(2)}value: ${ctlId}.values.indexOf(${valueExpr})`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Phase 5: date + Calendar — MonthGrid / DayOfWeekRow (QtQuick.Templates 6.3)
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** Translate an onChange / onInput arrow handler for Calendar / date-input day-click events.
 *  Maps the synthetic DOM event `e.target.value` / `e.target.valueAsDate` to the QML expression
 *  `new Date(model.year, model.month, model.day)` — the JS Date for the cell that was clicked.
 *  `valueAsDate` is checked first (longer match) so the regex alternation is unambiguous. */
function translateDateClickHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope, dateExprOverride?: string): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  let body: string;
  if (t.isBlockStatement(fn.body)) {
    body = fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  } else {
    body = emitExpr(fn.body, inner);
  }
  const dateExpr = dateExprOverride ?? "new Date(model.year, model.month, model.day)";
  return body
    .replace(/__ev\.(?:currentTarget|target)\.valueAsDate/g, dateExpr)
    .replace(/__ev\.(?:currentTarget|target)\.value/g, dateExpr);
}

/** <Calendar value={expr} onChange={fn} class="…"> — an inline month-grid widget (our tag, not HTML).
 *
 *  Wrapper: Css.CssFill (cssPrimitive "div") — participates in the parent's flex/grid layout.
 *  implicitWidth/Height set to a sane fixed size (7 cells × 32 px wide; header 32 + DOW 24 + grid 6×32).
 *
 *  The calendar header, DayOfWeekRow and MonthGrid are hosted inside a plain `Item` so the CSS
 *  layout engine ignores them (only Css.* direct children are layout participants). The nav buttons
 *  and title ARE Css.* items but they live inside the Item, so the outer CSS layout does not
 *  reposition them; explicit x/y values govern their placement.
 *
 *  value: reactive `property var __calVal<n>` — the source of truth for the :selected highlight.
 *  Navigation: `__calMonth<n>` / `__calYear<n>` are initialized from value (binding) and then
 *  broken by the first nav click (standard QML property-binding semantics), after which they track
 *  the user's navigation position independently of the value signal.
 *
 *  NOTE: Calendar is in BUILTIN_TAGS so it shadows any user component named "Calendar". This is
 *  intentional — our widget is the canonical Calendar primitive (same precedence as Show/For). */
function emitCalendar(propsArg: t.Node | undefined, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);

  const props = readProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  counter.n++;

  // One .qml per component: instantiate W.Calendar (the MonthGrid body lives in Calendar.qml).
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  let valueExpr: string | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      if (p.key.name === "value" && t.isExpression(p.value))
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (p.key.name === "onChange" && t.isExpression(p.value)
        && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
    }
  }

  // Picking a day fires dayPicked(date); translate the author's onChange so e.target.value → `date`.
  const pickedBody = onChangeFn ? translateDateClickHandler(onChangeFn, scope, "date") : "";

  const lines: string[] = [
    `${pad}W.Calendar {`,
    ...classLine,
    ...guardLine(guard, level),
  ];
  if (valueExpr !== null) lines.push(`${i(1)}value: ${valueExpr}`);
  if (pickedBody) lines.push(`${i(1)}onDayPicked: (date) => { ${pickedBody} }`);
  lines.push(`${pad}}`);
  return lines;
}

/** <input type="date" value={expr} onChange={fn}> → readOnly text field (formatted via Qt.formatDate)
 *  with a calendar-glyph chevron and a T.Popup containing the shared month-grid structure.
 *
 *  - The wrapper Css.CssFill carries CSS identity (cssPrimitive "input") and `:focus` when the
 *    popup is open, `:disabled` when the text field is disabled.
 *  - A Binding persistently formats the signal value into the T.TextField text via Qt.formatDate
 *    (ISO format "yyyy-MM-dd") — the same restoreMode: RestoreNone idiom as other input widgets.
 *  - Picking a day in the popup fires the onChange synthetic Date event and closes the popup.
 *  - min/max attrs are NOT supported and throw a clear error (loud failure, like Phase 4 dynamic options).
 *  - The `ctlId` allocated by emitInput is reused as the T.TextField id to keep counter-monotonicity. */
function emitDateInput(
  propsArg: t.Node | undefined,
  props: Props,
  scope: Scope,
  level: number,
  guard: string | undefined,
  ctlId: string,
): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));

  // min/max are ISO date range constraints — out of scope for this plan.
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      if (p.key.name === "min" || p.key.name === "max")
        throw new Error(`<input type="date"> min/max not supported yet — validate the date range in the onChange handler`);
    }
  }

  // One .qml per component: instantiate W.DateField (the readOnly field + chevron + popup + shared
  // MonthGrid live in DateField.qml). The counter slot allocated by emitInput is consumed so sibling
  // widgets stay monotonically numbered; DateField needs no external id (its state is self-contained).
  void ctlId;

  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  let valueExpr: string | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let disabled = false;

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value))
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (key === "onChange" && t.isExpression(p.value)
        && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "disabled") disabled = !t.isBooleanLiteral(p.value) || p.value.value;
    }
  }

  // Picking a day (or committing the keyboard cursor) fires dayPicked(date); translate the author's
  // onChange so e.target.value / e.target.valueAsDate → `date`.
  const pickedBody = onChangeFn ? translateDateClickHandler(onChangeFn, scope, "date") : "";

  const lines: string[] = [
    `${pad}W.DateField {`,
    ...classLine,
    ...guardLine(guard, level),
  ];
  if (valueExpr !== null) lines.push(`${i(1)}value: ${valueExpr}`);
  if (disabled) lines.push(`${i(1)}enabled: false`);
  if (pickedBody) lines.push(`${i(1)}onDayPicked: (date) => { ${pickedBody} }`);
  lines.push(`${pad}}`);
  return lines;
}

/** Children that are elements (recurse) interleaved with text/interpolation runs (one CssText each). */
export function emitChildren(children: t.Node[], scope: Scope, level: number): string[] {
  const out: string[] = [];
  let run: t.Node[] = [];
  const flush = () => {
    if (!run.length) return;
    if (run.some((n) => !isWhitespaceString(n))) out.push(...textNode(run, scope, level));
    run = [];
  };
  for (const child of children) {
    if (isHCall(child)) {
      flush();
      out.push(...emitQml(child, scope, level));
    } else if (isChildrenMarker(child, scope)) {
      flush(); // slot marker emits nothing here — children mount via the default property
    } else {
      run.push(child);
    }
  }
  flush();
  return out;
}

function textNode(run: t.Node[], scope: Scope, level: number): string[] {
  const pad = INDENT.repeat(level);
  // Anonymous text runs are CSS text nodes, not authored <text> elements: they must ONLY inherit
  // (colour/font from the containing box) and must NOT match a `text {}` type selector. CssText
  // defaults cssPrimitive to "text", so we blank it explicitly — otherwise a generic `text {}` rule
  // would hijack raw text (e.g. a big styled "Q" in a div) away from its parent's inherited style.
  return [
    `${pad}Css.CssText {`,
    `${pad}${INDENT}cssPrimitive: ""`,
    `${pad}${INDENT}text: ${textBinding(run, scope)}`,
    `${pad}}`,
  ];
}

/** Build a string-valued binding from text + interpolation children: `"a" + (expr) + "b"`. */
function textBinding(children: t.Node[], scope: Scope): string {
  const parts: string[] = [];
  for (const child of children) {
    if (t.isStringLiteral(child)) {
      if (child.value.trim() || /\s/.test(child.value)) parts.push(JSON.stringify(child.value));
    } else if (isJsxText(child)) {
      const cleaned = (child as any).value.replace(/\s+/g, " ");
      if (cleaned.trim()) parts.push(JSON.stringify(cleaned));
    } else if (t.isExpression(child)) {
      parts.push(`(${emitExpr(child, { ...scope, mode: "binding" })})`);
    }
  }
  if (parts.length === 0) return `""`;
  if (parts.length === 1 && parts[0].startsWith('"')) return parts[0];
  return parts[0].startsWith('"') ? parts.join(" + ") : `"" + ${parts.join(" + ")}`;
}

function emitHandler(node: t.Node | undefined, scope: Scope): string | null {
  if (!node) return null;
  const handlerScope: Scope = { ...scope, mode: "handler" };
  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) {
    if (t.isBlockStatement(node.body)) throw new Error("block-bodied handler not supported in this plan");
    return emitExpr(node.body, handlerScope);
  }
  // A helper identifier as a handler — `onClick={add}` where `add` is a local helper function:
  // emit as `add()` (the handler calls the method). The method lives on the QML object.
  if (t.isIdentifier(node) && scope.helpers?.has(node.name)) {
    return `${safeName(node.name)}()`;
  }
  // A bare function reference as a handler — `onClick={counter.increment}` where counter ∈ ctxBindings:
  // resolve to the injected provider's fn member and CALL it (the handler invokes the ref).
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && scope.ctxBindings && node.object.name in scope.ctxBindings) {
    return `${emitExpr(node, handlerScope)}()`;
  }
  throw new Error("only inline arrow handlers (or a context fn-member reference) are supported in this plan");
}

function readProps(propsArg: t.Node | undefined): Props {
  const props: Props = { classes: [], classList: [], onClick: undefined, type: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
  if (!propsArg || !t.isObjectExpression(propsArg)) return props;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
    if (p.key.name === "class" && t.isStringLiteral(p.value)) props.classes = p.value.value.split(/\s+/).filter(Boolean);
    if (p.key.name === "classList" && t.isObjectExpression(p.value)) {
      for (const cp of p.value.properties) {
        if (!t.isObjectProperty(cp)) continue;
        const key = t.isIdentifier(cp.key) ? cp.key.name : t.isStringLiteral(cp.key) ? cp.key.value : null;
        if (key && t.isExpression(cp.value)) props.classList.push({ key, expr: cp.value });
      }
    }
    if (p.key.name === "onClick") props.onClick = p.value;
    if (p.key.name === "type" && t.isStringLiteral(p.value)) props.type = p.value.value;
    if (p.key.name === "draggable") props.draggable = !t.isBooleanLiteral(p.value) || p.value.value;
    if (p.key.name === "dragData" && t.isExpression(p.value)) props.dragData = p.value;
    if (p.key.name === "onDrop") props.onDrop = p.value;
    if (p.key.name === "ref" && t.isIdentifier(p.value)) props.ref = p.value.name;
  }
  return props;
}

function isWhitespaceString(n: t.Node): boolean {
  return (t.isStringLiteral(n) && !n.value.trim()) || (isJsxText(n) && !(n as any).value.trim());
}
function isJsxText(n: t.Node): boolean {
  return n.type === "JSXText";
}

/** `props.children` (the default-slot marker) — a member expr on the component's props param. */
function isChildrenMarker(n: t.Node, scope: Scope): boolean {
  return !!scope.propsParam && t.isMemberExpression(n) && !n.computed &&
    t.isIdentifier(n.object, { name: scope.propsParam }) && t.isIdentifier(n.property, { name: "children" });
}

/** <Show when={C} fallback={F}>children</Show>: emit the children gated by `visible: !!(C)`, and the
 *  fallback element(s) gated by the inverted guard `visible: !(C)`. A fragment fallback (h(hFrag,...))
 *  has each of its children individually inverted-gated. */
function emitShow(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, outerGuard?: string): string[] {
  const when = readWhen(propsArg, scope);
  const guard = outerGuard ? `(${outerGuard}) && (${when})` : when;
  const out: string[] = [];
  for (const child of children) {
    if (isHCall(child)) out.push(...emitQml(child, scope, level, guard));
  }
  // Emit fallback element(s) with the inverted guard
  const fallback = readFallback(propsArg, scope);
  for (const fb of fallback) {
    if (isHCall(fb)) {
      // Check for a fragment: h(hFrag, null, ...kids)
      const { tag: tagArg, children: fbKids } = hParts(fb);
      if (isFragmentTag(tagArg)) {
        for (const kid of fbKids) {
          if (isHCall(kid)) {
            out.push(...emitWithInvertedGuard(kid, scope, level, when, outerGuard));
          }
        }
      } else {
        out.push(...emitWithInvertedGuard(fb, scope, level, when, outerGuard));
      }
    }
  }
  return out;
}

/** Emit a single element with the inverted Show guard injected. We emit it normally (emitQml gives it
 *  its own structure) then insert the `visible:` line. Since emitQml uses a `guard` param which wraps
 *  in `!!()`, we instead emit the element WITHOUT a guard and manually splice the inverted-guard line
 *  at the right indent level (after the opening `{` line). */
/** <Suspense fallback={F}>children</Suspense>: gate children on "no tracked resource is loading"
 *  (`!(<r>_loading || …)`) and the fallback on the inverse (loading). Reuses the Show guard helpers.
 *  The tracked resource set comes from scope.resources (the component's createResource decls). */
function emitSuspense(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, outerGuard?: string): string[] {
  const loading = (scope.resources ?? []).map((r) => `${safeName(r)}_loading`);
  // "ready" = no tracked resource loading. With no resources, Suspense degenerates to always-ready.
  const when = loading.length ? `!(${loading.join(" || ")})` : "true";
  const guard = outerGuard ? `(${outerGuard}) && (${when})` : when;
  const out: string[] = [];
  for (const child of children) {
    if (isHCall(child)) out.push(...emitQml(child, scope, level, guard));
  }
  // Fallback shown while loading (the inverse of `when`).
  const fallback = readFallback(propsArg, scope);
  for (const fb of fallback) {
    if (isHCall(fb)) {
      const { tag: tagArg, children: fbKids } = hParts(fb);
      if (isFragmentTag(tagArg)) {
        for (const kid of fbKids)
          if (isHCall(kid)) out.push(...emitWithInvertedGuard(kid, scope, level, when, outerGuard));
      } else {
        out.push(...emitWithInvertedGuard(fb, scope, level, when, outerGuard));
      }
    }
  }
  return out;
}

function emitWithInvertedGuard(call: t.CallExpression, scope: Scope, level: number, when: string, outerGuard?: string): string[] {
  // Emit the element without any guard, then inject the inverted visible line after the first line.
  const lines = emitQml(call, scope, level);
  const visibleLines = invertedGuardLine(when, outerGuard, level);
  // Insert the visible: line right after the opening brace line (index 0).
  lines.splice(1, 0, ...visibleLines);
  return lines;
}

function readFallback(propsArg: t.Node | undefined, scope: Scope): t.Node[] {
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "fallback" }) && t.isExpression(p.value))
        return [p.value];
    }
  }
  return [];
}

function readWhen(propsArg: t.Node | undefined, scope: Scope): string {
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "when" }) && t.isExpression(p.value))
        return emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  return "true";
}

/** <For each={E}>{(item) => …}</For> → Css.CssRepeater { model: E; delegate: Component {…} }.
 *  KEYED like Solid's <For>: a changed array reuses/moves surviving rows (flyweight) instead
 *  of tearing everything down the way a plain Repeater does. */
function emitFor(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const each = readEach(propsArg, scope);
  const pad = INDENT.repeat(level);
  const lines = [`${pad}Css.CssRepeater {`, ...guardLine(guard, level), `${pad}${INDENT}model: ${each}`];
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (delegate) {
    const param = delegate.params[0];
    const itemName = param && t.isIdentifier(param) ? param.name : null;
    const inner: Scope = { ...scope, locals: { ...(scope.locals ?? {}), ...(itemName ? { [itemName]: "modelData" } : {}) } };
    const body = delegate.body;
    if (!isHCall(body)) throw new Error("For delegate must return a single element in this plan");
    lines.push(`${pad}${INDENT}delegate: Component {`);
    lines.push(...emitQml(body, inner, level + 2));
    lines.push(`${pad}${INDENT}}`);
  }
  lines.push(`${pad}}`);
  return lines;
}

/** <Index each={E}>{(item, i) => …}</Index> → Repeater { model: E; delegate }. Unlike <For>, the
 *  row `item` is a zero-arg accessor (item() → modelData) and `i` is the index number. */
function emitIndex(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const each = readEach(propsArg, scope);
  const pad = INDENT.repeat(level);
  const lines = [`${pad}Repeater {`, ...guardLine(guard, level), `${pad}${INDENT}model: ${each}`];
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (delegate) {
    const [p0, p1] = delegate.params;
    const itemName = p0 && t.isIdentifier(p0) ? p0.name : null;
    const idxName = p1 && t.isIdentifier(p1) ? p1.name : null;
    const inner: Scope = {
      ...scope,
      accessors: { ...(scope.accessors ?? {}), ...(itemName ? { [itemName]: "modelData" } : {}) },
      locals: { ...(scope.locals ?? {}), ...(idxName ? { [idxName]: "index" } : {}) },
    };
    const body = delegate.body;
    if (isHCall(body)) lines.push(...emitQml(body, inner, level + 1));
    else throw new Error("Index delegate must return a single element in this plan");
  }
  lines.push(`${pad}}`);
  return lines;
}

/** <Switch><Match when={C}>…</Match>…</Switch> → LAZY, matching Solid's mount/unmount: only the
 *  first matching Match is instantiated; non-matching branches are NOT created (and are destroyed
 *  when their guard flips false). Each Match child becomes a `Repeater { model: (guard) ? 1 : 0 }`
 *  whose delegate is the branch content. A Repeater reparents its delegates to the containing
 *  CssRect's contentHolder (the same mechanism <For> uses), so the branch stays a DIRECT layout
 *  child of the box and the CssLayoutEngine sizes it exactly as before — a `Loader` would NOT
 *  (csslayout.cpp isLayoutChild only recognises children exposing style/cssPrimitive). Guard/
 *  first-wins semantics are unchanged; only the gate goes from `visible` (eager) to `model` (lazy). */
function emitSwitch(children: t.Node[], scope: Scope, level: number, outerGuard?: string): string[] {
  const out: string[] = [];
  const priors: string[] = [];
  const pad = INDENT.repeat(level);
  for (const c of children) {
    if (!isHCall(c)) continue;
    const { tag: tagArg, props: propsArg, children: kids } = hParts(c);
    if (!t.isIdentifier(tagArg, { name: "Match" })) continue;
    const when = readWhen(propsArg, scope);
    const cond = priors.length ? `(${when}) && !(${priors.join(" || ")})` : when;
    const guard = outerGuard ? `(${outerGuard}) && (${cond})` : cond;
    for (const k of kids) {
      if (!isHCall(k)) continue;
      // Async branch mount: the click flips `active`, the page INCUBATES (time-sliced by the
      // window's incubation controller) and the ready item is reparented into the content
      // holder as a direct layout child — Repeater semantics, without blocking the frame.
      out.push(`${pad}Css.CssIncubator {`, `${pad}${INDENT}active: (${guard}) ? true : false`);
      out.push(`${pad}${INDENT}sourceComponent: Component {`);
      out.push(...emitQml(k, scope, level + 2));
      out.push(`${pad}${INDENT}}`, `${pad}}`);
    }
    priors.push(`(${when})`);
  }
  return out;
}

function readEach(propsArg: t.Node | undefined, scope: Scope): string {
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "each" }) && t.isExpression(p.value))
        return emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  return "[]";
}

/** <Dynamic component={E}>text</Dynamic> → a CssText whose cssPrimitive is the (reactive) tag E. */
function emitDynamic(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  let comp = "div";
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "component" }) && t.isExpression(p.value))
        comp = emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  const pad = INDENT.repeat(level);
  return [
    `${pad}Css.CssText {`,
    ...guardLine(guard, level),
    `${pad}${INDENT}cssPrimitive: ${comp}`,
    `${pad}${INDENT}text: ${textBinding(children, scope)}`,
    `${pad}}`,
  ];
}
