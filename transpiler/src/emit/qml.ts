import * as t from "@babel/types";
import { emitExpr, type Scope } from "./expr.ts";
import { safeName } from "../names/safe.ts";
import { CONTROL_TAGS, hParts, isFragmentTag, isHCall } from "../ast/h.ts";

const INDENT = "    ";
const TEXT_TAGS = new Set(["text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

interface Props {
  classes: string[];
  classList: Array<{ key: string; expr: t.Expression }>;
  onClick: t.Node | undefined;
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
function buildCssClassLine(props: Props, scope: Scope, pad: string): string[] {
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
  if (tag === "input") return emitInput(propsArg, props, scope, level, guard);
  if (tag === "textarea") return emitTextarea(propsArg, props, scope, level, guard);
  if (tag === "select") return emitSelect(propsArg, props, children as t.Node[], scope, level, guard);

  if (TEXT_TAGS.has(tag)) {
    return [
      `${pad}Css.CssText {`,
      ...classLine,
      ...guardLine(guard, level),
      `${pad}${INDENT}cssPrimitive: ${JSON.stringify(tag)}`,
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
  return [
    `${pad}Css.CssRect {`,
    ...classLine,
    ...stateLine,
    ...refLine,
    ...guardLine(guard, level),
    `${pad}${INDENT}cssPrimitive: ${JSON.stringify(tag)}`,
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

function guardLine(guard: string | undefined, level: number): string[] {
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
    `${pad}}`,
  );
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
  // The MouseArea doubles as the hover tracker: `cssState` mirrors containsMouse so
  // `:hover` rules restyle the button (and, via ancestor scoping, its label) natively.
  const counter = scope.hoverCounter ?? { n: 0 };
  const maId = `__hover${counter.n++}`;
  const lines = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssState: ${maId}.containsMouse ? ["hover"] : []`,
    `${i(1)}cssPrimitive: "button"`,
    `${i(1)}Css.CssText {`,
    `${i(2)}cssPrimitive: "text"`,
    `${i(2)}text: ${textBinding(textKids, scope)}`,
    `${i(1)}}`,
    ...emitChildren(elemKids, scope, level + 1),
    `${i(1)}MouseArea {`,
    `${i(2)}id: ${maId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}hoverEnabled: true`,
    `${i(2)}cursorShape: Qt.PointingHandCursor`,
  ];
  const handler = emitHandler(props.onClick, scope);
  if (handler) lines.push(`${i(2)}onClicked: ${handler}`);
  lines.push(`${i(1)}}`, `${pad}}`);
  return lines;
}

/** <img class="a" src={u} /> → Css.CssImage { source: <src> || "" } (CssImage does object-fit +
 *  rounded-rect clip via MultiEffect, so `border-radius` yields a real circular avatar). */
function emitImage(propsArg: t.Node | undefined, props: Props, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  // Resolve the `src` prop via emitExpr in binding mode.
  let src = '""';
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "src" }) && t.isExpression(p.value))
        src = emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  return [
    `${pad}Css.CssImage {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}source: ${src} || ""`,
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

/** Emit the color + font bindings that bridge the CssFill parent's CSS-inherited properties into a
 *  native text control (T.TextField or T.TextArea). The CssFill exposes `inheritedColor`,
 *  `inheritedFontFamily`, and `inheritedFontSize` as resolved CSS string values; cssTheme helpers
 *  parse them into the QML types the native control expects. Fallbacks match existing gallery defaults. */
function widgetColorFont(i: (n: number) => string, extraIndent = 0, source = "parent"): string[] {
  return [
    `${i(2 + extraIndent)}color: cssTheme.parseColor(${source}.inheritedColor || "#2b2b2b")`,
    `${i(2 + extraIndent)}font.family: cssTheme.resolveFontFamily(${source}.inheritedFontFamily || "Sans Serif")`,
    `${i(2 + extraIndent)}font.pixelSize: cssTheme.parseFontSize(${source}.inheritedFontSize || "13px", 13)`,
  ];
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
  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;

  // Mark that this component uses QtQuick.Templates so the header emits the import.
  if (scope.usedWidgets) scope.usedWidgets.flag = true;

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

  // cssState: `:focus` when the control has keyboard focus; `:disabled` when not enabled.
  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  // Handler bodies — translateInputHandler maps e.target.value → `text`, e.key → event.key, etc.
  let textEditedBody = onInputFn
    ? translateInputHandler(onInputFn, scope)
    : signalName ? `${safeName(signalName)} = text` : "";
  const editingFinishedBody = onChangeFn ? translateInputHandler(onChangeFn, scope) : "";
  const keyBody = onKeyDownFn ? translateInputHandler(onKeyDownFn, scope) : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${cssState}`,
    // Implicit size from the control so flex/grid overrides still work when no CSS size is set.
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.TextField {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    // No visual chrome from Templates; our CssFill owns every painted pixel.
    `${i(2)}background: null`,
    ...widgetColorFont(i),
    `${i(2)}leftPadding: 12`,
    `${i(2)}rightPadding: 12`,
    `${i(2)}verticalAlignment: TextInput.AlignVCenter`,
    `${i(2)}selectByMouse: true`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
  ];

  if (type === "password") lines.push(`${i(2)}echoMode: TextInput.Password`);
  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (readOnly) lines.push(`${i(2)}readOnly: true`);
  if (maxLength !== null) lines.push(`${i(2)}maximumLength: ${maxLength}`);
  if (textEditedBody) lines.push(`${i(2)}onTextEdited: { ${textEditedBody} }`);
  if (editingFinishedBody) lines.push(`${i(2)}onEditingFinished: { ${editingFinishedBody} }`);
  if (keyBody) lines.push(`${i(2)}Keys.onPressed: (event) => { ${keyBody} }`);

  // Placeholder: a plain Text overlay inside the control (positioned to match the text baseline).
  // Hides when the field has text or is focused (web `<input>` placeholder semantics).
  if (placeholder) {
    lines.push(
      `${i(2)}Text {`,
      `${i(3)}anchors.verticalCenter: parent.verticalCenter`,
      `${i(3)}anchors.left: parent.left`,
      `${i(3)}anchors.leftMargin: parent.leftPadding`,
      `${i(3)}visible: parent.text.length === 0 && !parent.activeFocus`,
      `${i(3)}text: ${JSON.stringify(placeholder)}`,
      `${i(3)}color: "#9aa0a6"`,
      `${i(3)}font: parent.font`,
      `${i(2)}}`,
    );
  }

  lines.push(`${i(1)}}`);

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

  if (scope.usedWidgets) scope.usedWidgets.flag = true;

  const { valueExpr, signalName, placeholder, onInputFn, onChangeFn, disabled, readOnly }
    = readWidgetProps(propsArg, scope);

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  // T.TextArea has no textEdited signal: map both onInput and onChange to onTextChanged.
  // (onInput takes priority if both are specified.)
  const fn = onInputFn ?? onChangeFn;
  let textChangedBody = fn
    ? translateInputHandler(fn, scope)
    : signalName ? `${safeName(signalName)} = text` : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "textarea"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.TextArea {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    // Allow the text to wrap; callers can override via CSS `white-space: nowrap` (not yet mapped).
    `${i(2)}wrapMode: TextEdit.Wrap`,
    ...widgetColorFont(i),
    `${i(2)}padding: 12`,
    `${i(2)}selectByMouse: true`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (readOnly) lines.push(`${i(2)}readOnly: true`);
  if (textChangedBody) lines.push(`${i(2)}onTextChanged: { ${textChangedBody} }`);

  // Placeholder: overlaid at the top-left of the editing area (top-aligned for multi-line).
  if (placeholder) {
    lines.push(
      `${i(2)}Text {`,
      `${i(3)}anchors.top: parent.top`,
      `${i(3)}anchors.left: parent.left`,
      `${i(3)}anchors.topMargin: parent.padding`,
      `${i(3)}anchors.leftMargin: parent.padding`,
      `${i(3)}visible: parent.text.length === 0 && !parent.activeFocus`,
      `${i(3)}text: ${JSON.stringify(placeholder)}`,
      `${i(3)}color: "#9aa0a6"`,
      `${i(3)}font: parent.font`,
      `${i(2)}}`,
    );
  }

  lines.push(`${i(1)}}`);

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
  const checkState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(${ctlId}.checked ? ["checked"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${checkState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.CheckBox {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    `${i(2)}contentItem: null`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    // Arrows move focus along the chain (desktop dialog semantics), same opt-out as Tab.
    `${i(2)}Keys.onDownPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onRightPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onUpPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onLeftPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    // indicator: a fixed-size Css item (not in a Css layout container — geometry is hardcoded).
    `${i(2)}indicator: Css.CssFill {`,
    `${i(3)}cssPrimitive: "span"`,
    `${i(3)}cssClass: ["indicator"]`,
    `${i(3)}cssState: ${checkState}`,
    `${i(3)}width: 20`,
    `${i(3)}height: 20`,
    `${i(3)}implicitWidth: 20`,
    `${i(3)}implicitHeight: 20`,
    // Anchored Item host: once the indicator's CSS carries box rules (border etc.) the
    // layout engine runs a flex pass over contentHolder children and pins plain children
    // top-left; an anchors.fill Item is skipped, and anchors hold inside it. The nested
    // CssItem injects color/font from the .indicator-glyph rule without joining any layout.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Text {`,
    `${i(5)}text: "✓"`,
    `${i(5)}visible: ${ctlId}.checked`,
    `${i(5)}anchors.centerIn: parent`,
    `${i(5)}Css.CssItem { cssPrimitive: "text"; cssClass: ["indicator-glyph"] }`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (onChangeFn) lines.push(`${i(2)}onToggled: { ${translateToggleHandler(onChangeFn, ctlId, scope)} }`);

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

/** <input type="checkbox" role="switch"> → wrapper CssFill + T.Switch.
 *  The indicator slot hosts a track CssFill (36×20) containing a knob CssRect (16×16) whose
 *  x position is animated by T.Switch.visualPosition (0→1). The Behavior on x gives a 120 ms
 *  slide without requiring a CSS transition (CSS animations are not yet wired). */
function emitSwitchToggle(props: Props, scope: Scope, level: number, guard: string | undefined, ctlId: string, wp: ReturnType<typeof readWidgetProps>): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));
  const { checkedExpr, onChangeFn, disabled } = wp;
  const switchState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(${ctlId}.checked ? ["checked"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${switchState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.Switch {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    `${i(2)}contentItem: null`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    // Arrows move focus along the chain (desktop dialog semantics), same opt-out as Tab.
    `${i(2)}Keys.onDownPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onRightPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onUpPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}Keys.onLeftPressed: { if (solidTabstop.enabled) { var __n = ${ctlId}.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }`,
    `${i(2)}indicator: Css.CssFill {`,
    `${i(3)}cssPrimitive: "span"`,
    `${i(3)}cssClass: ["track"]`,
    `${i(3)}cssState: ${switchState}`,
    `${i(3)}width: 36`,
    `${i(3)}height: 20`,
    `${i(3)}implicitWidth: 36`,
    `${i(3)}implicitHeight: 20`,
    // Anchored Item host insulates the knob from the CSS flex pass (see the checkbox
    // glyph note); the geometry bindings live on the plain Rectangle inside. The nested
    // CssItem injects background-color/radius/border from the .knob rule.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Rectangle {`,
    `${i(5)}width: 16`,
    `${i(5)}height: 16`,
    `${i(5)}radius: 8`,
    `${i(5)}color: "#ffffff"`,
    `${i(5)}y: (parent.height - height) / 2`,
    // visualPosition goes 0→1 as the switch toggles; multiply by the remaining track width.
    `${i(5)}x: ${ctlId}.visualPosition * (parent.width - width)`,
    `${i(5)}Behavior on x { NumberAnimation { duration: 120 } }`,
    `${i(5)}Css.CssItem { cssPrimitive: "rect"; cssClass: ["knob"] }`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (onChangeFn) lines.push(`${i(2)}onToggled: { ${translateToggleHandler(onChangeFn, ctlId, scope)} }`);

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
  const radioState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(${ctlId}.checked ? ["checked"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;

  // Register this group name so emitComponentType can emit T.ButtonGroup { id: __group_<name> }.
  if (name && scope.buttonGroups) scope.buttonGroups.add(name);
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
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
  ];

  // Attach to the group if a name was given; the group is declared at root level by emitComponentType.
  if (groupId) lines.push(`${i(2)}T.ButtonGroup.group: ${groupId}`);

  // Arrow keys step the radio group (HTML/desktop semantics): focus AND check the
  // neighbour, wrapping. toggled() re-fires so the author's onChange wiring runs.
  if (groupId) lines.push(
    `${i(2)}function __step(d) { var bs = ${groupId}.buttons; var j = (bs.indexOf(${ctlId}) + d + bs.length) % bs.length; var b = bs[j]; b.forceActiveFocus(Qt.TabFocusReason); b.checked = true; b.toggled() }`,
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
  if (onChangeFn) lines.push(`${i(2)}onToggled: { ${translateToggleHandler(onChangeFn, ctlId, scope)} }`);

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

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  // Both onInput and onChange map to onMoved (see jsdoc for the approximation note).
  const activeFn = onInputFn ?? onChangeFn;
  const onMovedBody = activeFn
    ? translateValueHandler(activeFn, `${ctlId}.value`, scope)
    : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.Slider {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    // Track: background slot (fills the control); centred vertically via explicit x/y bindings.
    // Using Qt Basic-style geometry so the 6px-tall track sits in the middle of the taller handle.
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["track"]`,
    `${i(3)}x: ${ctlId}.leftPadding`,
    `${i(3)}y: ${ctlId}.topPadding + (${ctlId}.availableHeight - height) / 2`,
    `${i(3)}width: ${ctlId}.availableWidth`,
    `${i(3)}height: 6`,
    `${i(3)}implicitHeight: 6`,
    // Progress fill: a CssRect inside the track growing with visualPosition.
    `${i(3)}Css.CssRect {`,
    `${i(4)}cssClass: ["track-fill"]`,
    `${i(4)}width: ${ctlId}.visualPosition * parent.width`,
    `${i(4)}height: parent.height`,
    `${i(3)}}`,
    `${i(2)}}`,
    // Handle: a CssRect (18×18) positioned by the slider's own geometry helpers. No Behavior —
    // dragging must follow the pointer 1:1 (no snap animation like the Switch knob).
    `${i(2)}handle: Css.CssRect {`,
    `${i(3)}cssClass: ["handle"]`,
    `${i(3)}width: 18`,
    `${i(3)}height: 18`,
    `${i(3)}implicitWidth: 18`,
    `${i(3)}implicitHeight: 18`,
    `${i(3)}x: ${ctlId}.leftPadding + ${ctlId}.visualPosition * (${ctlId}.availableWidth - width)`,
    `${i(3)}y: ${ctlId}.topPadding + ${ctlId}.availableHeight / 2 - height / 2`,
    `${i(2)}}`,
    `${i(2)}from: ${min}`,
    `${i(2)}to: ${max}`,
    `${i(2)}stepSize: ${step}`,
    // Focused wheel steps the value (same semantics as the SpinBox); moved() re-fires
    // so the author's onInput/onChange wiring runs.
    // Same touchpad handling as the SpinBox wheel (see there): Mouse-only default +
    // 120-unit notch accumulation.
    `${i(2)}WheelHandler {`,
    `${i(3)}property real __acc: 0`,
    `${i(3)}enabled: ${ctlId}.activeFocus`,
    `${i(3)}acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad`,
    `${i(3)}onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ${ctlId}.value = Math.max(${ctlId}.from, Math.min(${ctlId}.to, ${ctlId}.value + s * ${ctlId}.stepSize)); ${ctlId}.moved() } }`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (onMovedBody) lines.push(`${i(2)}onMoved: { ${onMovedBody} }`);

  lines.push(`${i(1)}}`);

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

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  const onValueModifiedBody = onChangeFn
    ? translateValueHandler(onChangeFn, `${ctlId}.value`, scope)
    : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.SpinBox {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    `${i(2)}from: ${min}`,
    `${i(2)}to: ${max}`,
    `${i(2)}stepSize: ${step}`,
    `${i(2)}editable: true`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    // Controls resize contentItem to the control minus paddings — without a rightPadding
    // the TextInput covers the +/- buttons and eats their clicks.
    `${i(2)}leftPadding: 12`,
    `${i(2)}rightPadding: 32`,
    // HTML semantics: the wheel steps the value, but ONLY while the field has focus;
    // unfocused, the event must fall through to the page scroll. valueModified() reuses
    // the onChange wiring. Stepping writes `value` directly: Qt 6.11's SpinBox refactor
    // (QQuickAbstractSpinBox) dropped the Q_INVOKABLE from increase()/decrease() — they
    // no longer exist from QML and the call was a silent TypeError.
    // acceptedDevices: the default is Mouse ONLY — touchpad scrolling (system-synthesized
    // wheel) is filtered in wantsPointerEvent, so laptops never stepped. Touchpads also
    // send small continuous deltas (or pixelDelta with angleDelta 0): accumulate to the
    // 120-unit notch before stepping.
    `${i(2)}WheelHandler {`,
    `${i(3)}property real __acc: 0`,
    `${i(3)}enabled: ${ctlId}.activeFocus`,
    `${i(3)}acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad`,
    `${i(3)}onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ${ctlId}.value = Math.max(${ctlId}.from, Math.min(${ctlId}.to, ${ctlId}.value + s * ${ctlId}.stepSize)); ${ctlId}.valueModified() } }`,
    `${i(2)}}`,
    // contentItem: a plain TextInput (not Css) — it lives inside the control's item tree, not our
    // CSS layout engine. Color/font are bridged from the CssFill wrapper via ctlId.parent.inheritedX.
    `${i(2)}contentItem: TextInput {`,
    // T.SpinBox is a focus scope: focus: true forwards the control's active focus into the
    // TextInput so tabbing in lets the user type immediately.
    `${i(3)}focus: true`,
    `${i(3)}text: ${ctlId}.displayText`,
    `${i(3)}validator: ${ctlId}.validator`,
    `${i(3)}readOnly: !${ctlId}.editable`,
    `${i(3)}color: cssTheme.parseColor(${ctlId}.parent.inheritedColor || "#2b2b2b")`,
    `${i(3)}font.family: cssTheme.resolveFontFamily(${ctlId}.parent.inheritedFontFamily || "Sans Serif")`,
    `${i(3)}font.pixelSize: cssTheme.parseFontSize(${ctlId}.parent.inheritedFontSize || "13px", 13)`,
    `${i(3)}horizontalAlignment: Qt.AlignHCenter`,
    `${i(3)}verticalAlignment: Qt.AlignVCenter`,
    `${i(3)}selectByMouse: true`,
    `${i(2)}}`,
    // up indicator: Css.CssFill at the top-right of the SpinBox; cssState "active" when
    // pressed. Inset 2px from the wrapper's edge so the buttons sit INSIDE the rounded
    // border instead of overlapping it (they looked clipped at the corner).
    `${i(2)}up.indicator: Css.CssFill {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["spin-up"]`,
    `${i(3)}cssState: ${ctlId}.up.pressed ? ["active"] : []`,
    `${i(3)}x: parent.width - width - 2`,
    `${i(3)}y: 2`,
    `${i(3)}width: 24`,
    `${i(3)}height: (parent.height - 4) / 2`,
    `${i(3)}implicitWidth: 24`,
    `${i(3)}implicitHeight: (parent.height - 4) / 2`,
    // Plain Text, NOT CssText: a Css child inside this CssFill is re-laid-out by the CSS
    // engine (isLayoutChild is true for every Css type), which stomps the centerIn anchor.
    // A plain primitive is invisible to the layout; the nested CssItem injects the CSS
    // (color/font from the .spin-glyph rule) without joining the layout.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Text {`,
    `${i(5)}text: "+"`,
    `${i(5)}anchors.centerIn: parent`,
    `${i(5)}Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
    // down indicator: mirrors up, at the bottom-right (same 2px inset).
    `${i(2)}down.indicator: Css.CssFill {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["spin-down"]`,
    `${i(3)}cssState: ${ctlId}.down.pressed ? ["active"] : []`,
    `${i(3)}x: parent.width - width - 2`,
    `${i(3)}y: parent.height / 2`,
    `${i(3)}width: 24`,
    `${i(3)}height: (parent.height - 4) / 2`,
    `${i(3)}implicitWidth: 24`,
    `${i(3)}implicitHeight: (parent.height - 4) / 2`,
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Text {`,
    `${i(5)}text: "−"`,
    `${i(5)}anchors.centerIn: parent`,
    `${i(5)}Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (onValueModifiedBody) lines.push(`${i(2)}onValueModified: { ${onValueModifiedBody} }`);

  lines.push(`${i(1)}}`);

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
  const delId = `__optDel${idx}`;

  if (scope.usedWidgets) { scope.usedWidgets.flag = true; scope.usedWidgets.popupWindow = true; }

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

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  // onActivated(index): translate onChange so e.target.value → __values[index].
  const onActivatedBody = onChangeFn
    ? translateValueHandler(onChangeFn, `${ctlId}.__values[index]`, scope)
    : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "select"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.ComboBox {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    // No visual chrome from Templates; the CssFill wrapper owns the box painting.
    `${i(2)}background: null`,
    // leftPadding keeps the contentItem text clear of the border (G4 from Phase 1 probe).
    `${i(2)}leftPadding: 12`,
    // Parallel values array alongside the display-label model.
    `${i(2)}readonly property var __values: ${valuesArr}`,
    `${i(2)}model: ${modelArr}`,
    // contentItem: CssText showing the selected item's display label.
    `${i(2)}contentItem: Css.CssText {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["value"]`,
    `${i(3)}text: ${ctlId}.displayText`,
    `${i(2)}}`,
    // Chevron: absolutely positioned at the right-centre of the ComboBox.
    `${i(2)}Css.CssText {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["chevron"]`,
    `${i(3)}text: "▾"`,
    `${i(3)}anchors.right: parent.right`,
    `${i(3)}anchors.rightMargin: 8`,
    `${i(3)}anchors.verticalCenter: parent.verticalCenter`,
    `${i(2)}}`,
    // Delegate: one T.ItemDelegate per model row.
    `${i(2)}delegate: T.ItemDelegate {`,
    `${i(3)}id: ${delId}`,
    // The style must bind highlighted itself (Basic does the same) — without it keyboard
    // navigation moves highlightedIndex invisibly and the active row never changes.
    `${i(3)}highlighted: ${ctlId}.highlightedIndex === index`,
    // Width must be explicit (G4 from Phase 1): ComboBox does not size delegates automatically.
    `${i(3)}width: ${ctlId}.popup.width`,
    `${i(3)}implicitHeight: 36`,
    `${i(3)}background: Css.CssFill {`,
    `${i(4)}cssPrimitive: "div"`,
    `${i(4)}cssClass: ["option"]`,
    `${i(4)}cssState: (${delId}.highlighted ? ["hover"] : []).concat(${ctlId}.currentIndex === index ? ["selected"] : [])`,
    `${i(3)}}`,
    `${i(3)}contentItem: Css.CssText {`,
    `${i(4)}cssPrimitive: ""`,
    `${i(4)}cssClass: ["option-label"]`,
    `${i(4)}text: modelData`,
    `${i(3)}}`,
    `${i(2)}}`,
    // Popup: T.Popup below the control; padding ≥ border-width prevents clip (G3).
    // Templates popups have NO implicit-size policy of their own (that's the style's job,
    // and we ARE the style) — without this line the popup opens 0px tall.
    `${i(2)}popup: T.Popup {`,
    // Desktop dropdown: a REAL native window (escapes the app window bounds, Qt 6.8+),
    // flipping ABOVE the control when opening below would overflow the screen.
    `${i(3)}popupType: T.Popup.Window`,
    `${i(3)}y: (${ctlId}.mapToGlobal(0, ${ctlId}.height + 2).y + height > Screen.height) ? -(height + 2) : ${ctlId}.height + 2`,
    `${i(3)}width: ${ctlId}.width`,
    `${i(3)}implicitHeight: contentHeight + topPadding + bottomPadding`,
    `${i(3)}padding: 1`,
    // cssAncestor: popup contents are reparented to the window Overlay, severing the
    // visual chain `.wg-select .popup` matches against — re-anchor the engine's ancestor
    // walk at the control. background and contentItem are SIBLING slots; every popup
    // descendant's walk passes through one of them, so these two properties cover all rows.
    `${i(3)}background: Css.CssFill {`,
    `${i(4)}property Item cssAncestor: ${ctlId}`,
    `${i(4)}cssPrimitive: "div"`,
    `${i(4)}cssClass: ["popup"]`,
    `${i(3)}}`,
    `${i(3)}contentItem: ListView {`,
    `${i(4)}property Item cssAncestor: ${ctlId}`,
    `${i(4)}clip: true`,
    `${i(4)}model: ${ctlId}.delegateModel`,
    `${i(4)}currentIndex: ${ctlId}.highlightedIndex`,
    `${i(4)}implicitHeight: Math.min(contentHeight, 240)`,
    `${i(3)}}`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (onActivatedBody) lines.push(`${i(2)}onActivated: (index) => { ${onActivatedBody} }`);

  lines.push(`${i(1)}}`);

  // Binding: keep currentIndex in sync with the controlled value expression.
  if (valueExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "currentIndex"`,
      `${i(2)}value: ${ctlId}.__values.indexOf(${valueExpr})`,
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

/** Shared month-grid inner structure — emits lines for the calendar header (nav buttons + title),
 *  the DayOfWeekRow and the AbstractMonthGrid with its day delegate. The CALLER is responsible for
 *  emitting the surrounding container and the `__calVal<n>`, `__calMonth<n>`, `__calYear<n>`
 *  properties on it; all inline references use `ownerId.__calXXX<n>`.
 *
 *  @param ownerId - QML id of the item that owns __calVal/Month/Year (the calendar wrapper or date-field wrapper)
 *  @param n       - unique numeric suffix (from inputCounter) used for all generated ids in this calendar instance
 *  @param clickExtra - optional QML statement(s) appended after the onChange body (e.g. popup.close())
 *  @param onChangeFn - the author's onChange / onInput handler, or null
 *  @param level   - indentation level of the CONTAINER's children (the inner Item or popup contentItem)
 */
function emitMonthGridLines(
  ownerId: string,
  n: number,
  onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null,
  clickExtra: string,
  scope: Scope,
  level: number,
  cursorExpr?: string, // keyboard-cursor Date expr; adds a "focus" state to the matching cell
): string[] {
  const i = (d: number) => INDENT.repeat(level + d);
  const mgId = `__mg${n}`;
  const delId = `__mgDel${n}`;
  const dowId = `__dow${n}`;

  const clickBody = onChangeFn ? translateDateClickHandler(onChangeFn, scope) : "";
  const fullClick = [clickBody, clickExtra].filter(Boolean).join("; ");

  const selCheck =
    `${ownerId}.__calVal${n} instanceof Date` +
    ` && model.year === ${ownerId}.__calVal${n}.getFullYear()` +
    ` && model.month === ${ownerId}.__calVal${n}.getMonth()` +
    ` && model.day === ${ownerId}.__calVal${n}.getDate()`;
  const cursorCheck = cursorExpr
    ? `${cursorExpr} instanceof Date` +
      ` && model.year === ${cursorExpr}.getFullYear()` +
      ` && model.month === ${cursorExpr}.getMonth()` +
      ` && model.day === ${cursorExpr}.getDate()`
    : null;
  const dayCssState =
    `(model.today ? ["today"] : [])` +
    `.concat((${selCheck}) ? ["selected"] : [])` +
    `.concat(model.month !== ${mgId}.month ? ["outside"] : [])` +
    `.concat(${delId}.hovered ? ["hover"] : [])` +
    (cursorCheck ? `.concat((${cursorCheck}) ? ["focus"] : [])` : ``);

  return [
    // ── prev nav button ────────────────────────────────────────────────────
    `${i(0)}Css.CssFill {`,
    `${i(1)}cssPrimitive: "button"`,
    `${i(1)}cssClass: ["cal-nav"]`,
    `${i(1)}x: 0`,
    `${i(1)}y: 0`,
    `${i(1)}width: 32`,
    `${i(1)}height: 32`,
    `${i(1)}implicitWidth: 32`,
    `${i(1)}implicitHeight: 32`,
    `${i(1)}Css.CssText { cssPrimitive: ""; text: "‹"; anchors.centerIn: parent }`,
    `${i(1)}MouseArea {`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}onClicked: {`,
    `${i(3)}if (${ownerId}.__calMonth${n} === 0) { ${ownerId}.__calYear${n} = ${ownerId}.__calYear${n} - 1; ${ownerId}.__calMonth${n} = 11 }`,
    `${i(3)}else ${ownerId}.__calMonth${n} = ${ownerId}.__calMonth${n} - 1`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${i(0)}}`,
    // ── month/year label ───────────────────────────────────────────────────
    `${i(0)}Css.CssText {`,
    `${i(1)}cssPrimitive: ""`,
    `${i(1)}cssClass: ["cal-title"]`,
    `${i(1)}x: 32`,
    `${i(1)}y: 0`,
    `${i(1)}width: parent.width - 64`,
    `${i(1)}height: 32`,
    `${i(1)}text: Qt.locale().monthName(${ownerId}.__calMonth${n}) + " " + ${ownerId}.__calYear${n}`,
    // CssText does not expose horizontalAlignment/verticalAlignment as QML properties.
    // verticalAlignment is always AlignVCenter inside CssText (hardcoded in applyToText).
    // horizontal centering is driven by the style map key "text-align".
    `${i(1)}style: ({"text-align": "center"})`,
    `${i(0)}}`,
    // ── next nav button ────────────────────────────────────────────────────
    `${i(0)}Css.CssFill {`,
    `${i(1)}cssPrimitive: "button"`,
    `${i(1)}cssClass: ["cal-nav"]`,
    `${i(1)}x: parent.width - 32`,
    `${i(1)}y: 0`,
    `${i(1)}width: 32`,
    `${i(1)}height: 32`,
    `${i(1)}implicitWidth: 32`,
    `${i(1)}implicitHeight: 32`,
    `${i(1)}Css.CssText { cssPrimitive: ""; text: "›"; anchors.centerIn: parent }`,
    `${i(1)}MouseArea {`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}onClicked: {`,
    `${i(3)}if (${ownerId}.__calMonth${n} === 11) { ${ownerId}.__calYear${n} = ${ownerId}.__calYear${n} + 1; ${ownerId}.__calMonth${n} = 0 }`,
    `${i(3)}else ${ownerId}.__calMonth${n} = ${ownerId}.__calMonth${n} + 1`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${i(0)}}`,
    // ── day-of-week row ────────────────────────────────────────────────────
    // The Abstract templates instantiate NO delegates in C++ — the style (us) must supply a
    // contentItem whose Repeater binds control.source → control.delegate. The template's C++
    // then only sizes contentItem children (width/7); the Row/Grid positioner places them.
    `${i(0)}T.AbstractDayOfWeekRow {`,
    `${i(1)}id: ${dowId}`,
    `${i(1)}x: 0`,
    `${i(1)}y: 32`,
    `${i(1)}width: parent.width`,
    `${i(1)}height: 24`,
    `${i(1)}contentItem: Row {`,
    `${i(2)}Repeater {`,
    `${i(3)}model: ${dowId}.source`,
    `${i(3)}delegate: ${dowId}.delegate`,
    `${i(2)}}`,
    `${i(1)}}`,
    // Delegate host is a plain Item sized DECLARATIVELY with the template's own cell formula.
    // The C++ resizeItems() only fires on geometryChange — under CssIncubator the Repeater
    // populates after the last geometry change, so imperative sizing never lands and the Row
    // stacks 0-wide cells. A binding is timing-proof. The CssText centres inside the cell
    // (a bare CssText would re-assert its text-metrics size and misalign the Row).
    `${i(1)}delegate: Item {`,
    `${i(2)}width: (${dowId}.contentItem.width - 6 * ${dowId}.spacing) / 7`,
    `${i(2)}height: ${dowId}.contentItem.height`,
    `${i(2)}Css.CssText {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["dow"]`,
    `${i(3)}text: model.shortName`,
    `${i(3)}anchors.centerIn: parent`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${i(0)}}`,
    // ── month grid ─────────────────────────────────────────────────────────
    `${i(0)}T.AbstractMonthGrid {`,
    `${i(1)}id: ${mgId}`,
    `${i(1)}x: 0`,
    `${i(1)}y: 56`,
    `${i(1)}width: parent.width`,
    `${i(1)}height: parent.height - 56`,
    `${i(1)}month: ${ownerId}.__calMonth${n}`,
    `${i(1)}year: ${ownerId}.__calYear${n}`,
    `${i(1)}contentItem: Grid {`,
    `${i(2)}columns: 7`,
    `${i(2)}rows: 6`,
    `${i(2)}Repeater {`,
    `${i(3)}model: ${mgId}.source`,
    `${i(3)}delegate: ${mgId}.delegate`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${i(1)}delegate: T.AbstractButton {`,
    `${i(2)}id: ${delId}`,
    `${i(2)}implicitWidth: 32`,
    `${i(2)}implicitHeight: 32`,
    // Declarative cell size (same formula as the template's resizeItems) — see the
    // day-of-week delegate note: incubated creation misses the imperative resize.
    `${i(2)}width: (${mgId}.contentItem.width - 6 * ${mgId}.spacing) / 7`,
    `${i(2)}height: (${mgId}.contentItem.height - 5 * ${mgId}.spacing) / 6`,
    // background: a CssFill carrying the day's CSS class / pseudo-class state
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["day"]`,
    `${i(3)}cssState: ${dayCssState}`,
    `${i(2)}}`,
    // contentItem: the day number label. It carries the SAME state list as the background —
    // background and contentItem are sibling slots, so `.day:outside .day-label` can never
    // match (no ancestor relation); `.day-label:outside` does.
    `${i(2)}contentItem: Css.CssText {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["day-label"]`,
    `${i(3)}cssState: ${dayCssState}`,
    `${i(3)}text: model.day`,
    `${i(2)}}`,
    ...(fullClick ? [`${i(2)}onClicked: { ${fullClick} }`] : []),
    `${i(1)}}`,
    `${i(0)}}`,
  ];
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
  const n = counter.n++;

  if (scope.usedWidgets) { scope.usedWidgets.flag = true; scope.usedWidgets.calendar = true; }

  const calId = `__cal${n}`;

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

  const calValExpr = valueExpr ?? "null";

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}id: ${calId}`,
    `${i(1)}cssPrimitive: "div"`,
    // Fixed implicit size: 7 cells × 32 px wide; 32 (header) + 24 (DOW) + 6 rows × 32 = 280.
    `${i(1)}implicitWidth: 224`,
    `${i(1)}implicitHeight: 280`,
    // Reactive value property — source of truth for :selected highlight.
    `${i(1)}property var __calVal${n}: ${calValExpr}`,
    // View month/year initialized from value; binding breaks after the first nav click (QML semantics),
    // after which they track the user's navigation position independently.
    `${i(1)}property int __calMonth${n}: __calVal${n} instanceof Date ? __calVal${n}.getMonth() : new Date().getMonth()`,
    `${i(1)}property int __calYear${n}: __calVal${n} instanceof Date ? __calVal${n}.getFullYear() : new Date().getFullYear()`,
    // Plain Item insulates the calendar internals from the outer CSS layout engine (non-Css items
    // are skipped by isLayoutChild). The nav buttons / title / DOW / MonthGrid use explicit x/y.
    `${i(1)}Item {`,
    `${i(2)}anchors.fill: parent`,
    ...emitMonthGridLines(calId, n, onChangeFn, "", scope, level + 2),
    `${i(1)}}`,
    `${pad}}`,
  ];
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

  // Derive per-instance ids from the pre-allocated ctlId (avoids an extra counter increment).
  const wrapId = `${ctlId}W`;   // outer CssFill wrapper (carries state/props)
  const fldId  = ctlId;          // T.TextField (the text display control)
  const popId  = `${ctlId}P`;   // T.Popup (the calendar dropdown)
  // n used for month-grid sub-ids; extract from ctlId e.g. "__input3" → 3.
  const n = parseInt(ctlId.replace("__input", ""), 10);

  if (scope.usedWidgets) { scope.usedWidgets.flag = true; scope.usedWidgets.calendar = true; scope.usedWidgets.popupWindow = true; }

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

  const calValExpr = valueExpr ?? "null";
  // cssState: :focus when popup is open (visible = keyboard focus equivalent); :disabled from field.
  const cssState = `(${popId}.visible ? ["focus"] : []).concat(!${fldId}.enabled ? ["disabled"] : [])`;

  // Enter with the popup open commits the keyboard cursor through the SAME author onChange
  // path a cell click takes, then closes.
  const rawCommit = onChangeFn ? translateDateClickHandler(onChangeFn, scope, `__calCursor${n}`) : "";
  const commitBody = rawCommit && !rawCommit.trimEnd().endsWith(";") ? `${rawCommit}; ` : rawCommit ? `${rawCommit} ` : "";
  const commitFnLines = [
    `${i(1)}function __calCommit${n}() { if (!(__calCursor${n} instanceof Date)) return; ${commitBody}${popId}.close() }`,
  ];

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}id: ${wrapId}`,
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: 200`,
    `${i(1)}implicitHeight: 36`,
    // Reactive value property — source of truth for :selected highlight and formatted display.
    `${i(1)}property var __calVal${n}: ${calValExpr}`,
    `${i(1)}property int __calMonth${n}: __calVal${n} instanceof Date ? __calVal${n}.getMonth() : new Date().getMonth()`,
    `${i(1)}property int __calYear${n}: __calVal${n} instanceof Date ? __calVal${n}.getFullYear() : new Date().getFullYear()`,
    // Keyboard cursor: arrows move it by ±1 (left/right) and ±7 (up/down) days, following the
    // shown month; Enter commits it through the same onChange path a cell click uses.
    `${i(1)}property var __calCursor${n}: null`,
    `${i(1)}function __calStep${n}(days) { var b = __calCursor${n} instanceof Date ? __calCursor${n} : (__calVal${n} instanceof Date ? __calVal${n} : new Date()); var d = new Date(b.getFullYear(), b.getMonth(), b.getDate() + days); __calCursor${n} = d; __calMonth${n} = d.getMonth(); __calYear${n} = d.getFullYear() }`,
    ...(commitFnLines),
    // Anchored plain-Item host: the wrapper is a Css container, and once the author's CSS
    // gives it box rules (e.g. `.wg-date { padding: … }`) the layout engine runs a flex pass
    // over ALL contentHolder children — plain ones included — stretching the chevron Text to
    // the content box (measured: 1-glyph Text at width 560) and squeezing the TextField. An
    // anchors.fill Item is skipped by the layout; everything inside keeps its anchors. Same
    // insulation pattern as the inline <Calendar>.
    `${i(1)}Item {`,
    `${i(2)}anchors.fill: parent`,
    // ReadOnly text field — display only; typing dates is out of scope.
    `${i(2)}T.TextField {`,
    `${i(3)}id: ${fldId}`,
    `${i(3)}anchors.fill: parent`,
    `${i(3)}background: null`,
    `${i(3)}readOnly: true`,
    `${i(3)}activeFocusOnTab: solidTabstop.enabled`,
    // Keyboard: Enter/Space/Down open the popup; with it open, arrows move the day cursor
    // (±1 left/right, ±7 up/down — HTML date-picker semantics) and Enter/Space commit it.
    // The popup keeps focus on this field (focus: false default), so keys land here.
    `${i(3)}Keys.onReturnPressed: ${popId}.visible ? ${wrapId}.__calCommit${n}() : ${popId}.open()`,
    `${i(3)}Keys.onSpacePressed: ${popId}.visible ? ${wrapId}.__calCommit${n}() : ${popId}.open()`,
    `${i(3)}Keys.onDownPressed: ${popId}.visible ? ${wrapId}.__calStep${n}(7) : ${popId}.open()`,
    `${i(3)}Keys.onUpPressed: { if (${popId}.visible) ${wrapId}.__calStep${n}(-7) }`,
    `${i(3)}Keys.onLeftPressed: { if (${popId}.visible) ${wrapId}.__calStep${n}(-1) }`,
    `${i(3)}Keys.onRightPressed: { if (${popId}.visible) ${wrapId}.__calStep${n}(1) }`,
    ...widgetColorFont(i, 1, wrapId),
    `${i(3)}leftPadding: 12`,
    `${i(3)}rightPadding: 36`,
    `${i(3)}verticalAlignment: TextInput.AlignVCenter`,
  ];

  if (disabled) lines.push(`${i(3)}enabled: false`);
  lines.push(`${i(2)}}`);

  // Binding: keep the displayed text in sync with the signal value, formatted as ISO date.
  // restoreMode: RestoreNone — the binding survives popup open/close without reverting.
  lines.push(
    `${i(2)}Binding {`,
    `${i(3)}target: ${fldId}`,
    `${i(3)}property: "text"`,
    // Guard: Qt.formatDate throws on null/undefined (no Date object yet).
    `${i(3)}value: ${wrapId}.__calVal${n} instanceof Date ? Qt.formatDate(${wrapId}.__calVal${n}, "yyyy-MM-dd") : ""`,
    `${i(3)}restoreMode: Binding.RestoreNone`,
    `${i(2)}}`,
  );

  // Calendar glyph — a CssText IDENTICAL to the <select> chevron (same class, same CSS
  // box: `.chevron { padding-right; height }` applies), so the two dropdowns align.
  // Living inside the anchored host Item keeps it out of the wrapper's CSS layout pass.
  lines.push(
    `${i(2)}Css.CssText {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["chevron"]`,
    `${i(3)}text: "▾"`,
    `${i(3)}anchors.right: parent.right`,
    `${i(3)}anchors.rightMargin: 8`,
    `${i(3)}anchors.verticalCenter: parent.verticalCenter`,
    `${i(2)}}`,
  );

  // MouseArea over the whole field: click toggles the popup. CloseOnPressOutside fires on
  // the PRESS, so by the time the click lands here the popup already closed — a naive
  // visible-check reopens it. The popup stamps its close time; a click right after a
  // close (same interaction) is a toggle-close, not an open.
  lines.push(
    `${i(2)}MouseArea {`,
    `${i(3)}anchors.fill: parent`,
    `${i(3)}onClicked: { ${fldId}.forceActiveFocus(); if (${popId}.visible) ${popId}.close(); else if (Date.now() - ${popId}.__closedAt > 150) ${popId}.open() }`,
    `${i(2)}}`,
  );
  lines.push(`${i(1)}}`);

  // Popup: T.Popup renders on the window overlay; padding ≥ 1 prevents CssFill border clip (G3).
  // The contentItem is a plain Item holding the shared month-grid structure (same as Calendar).
  // Nav buttons inside the popup reference wrapId.__calMonth/Year (shared nav state).
  lines.push(
    `${i(1)}T.Popup {`,
    `${i(2)}id: ${popId}`,
    `${i(2)}property double __closedAt: 0`,
    `${i(2)}onOpened: ${wrapId}.__calCursor${n} = ${wrapId}.__calVal${n} instanceof Date ? ${wrapId}.__calVal${n} : new Date()`,
    `${i(2)}onClosed: { __closedAt = Date.now(); ${wrapId}.__calCursor${n} = null }`,
    // Desktop dropdown: native window + flip above on screen overflow (see emitSelect).
    `${i(2)}popupType: T.Popup.Window`,
    `${i(2)}y: (${wrapId}.mapToGlobal(0, ${wrapId}.height + 2).y + height > Screen.height) ? -(height + 2) : ${wrapId}.height + 2`,
    // Templates popups have no implicit-size policy (style's job — ours): without these
    // two lines the calendar dropdown opens 0x0.
    `${i(2)}implicitWidth: contentWidth + leftPadding + rightPadding`,
    `${i(2)}implicitHeight: contentHeight + topPadding + bottomPadding`,
    `${i(2)}padding: 1`,
    // cssAncestor: overlay reparenting severs the visual chain (see emitSelect) — re-anchor
    // at the date wrapper so `.wg-date .popup` / `.wg-date .day` keep matching.
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["popup"]`,
    `${i(2)}}`,
    `${i(2)}contentItem: Item {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}implicitWidth: 224`,
    `${i(3)}implicitHeight: 280`,
    // Shared month-grid structure: nav + DOW + MonthGrid; clicking a day fires onChange + close.
    ...emitMonthGridLines(wrapId, n, onChangeFn, `${popId}.close()`, scope, level + 3, `${wrapId}.__calCursor${n}`),
    `${i(2)}}`,
    `${i(1)}}`,
    `${pad}}`,
  );

  return lines;
}

/** Children that are elements (recurse) interleaved with text/interpolation runs (one CssText each). */
function emitChildren(children: t.Node[], scope: Scope, level: number): string[] {
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
  const props: Props = { classes: [], classList: [], onClick: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
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
