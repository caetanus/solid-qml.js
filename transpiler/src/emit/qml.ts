import * as t from "@babel/types";
import { emitExpr, shadowOwnParams, type Scope } from "./expr.ts";
import { emitStmt } from "./stmt.ts";
import { safeName } from "../names/safe.ts";
import { CONTROL_TAGS, hParts, isFragmentTag, isHCall } from "../ast/h.ts";
import { nativeTags, requireImport } from "./native/index.ts";
import { translateArgsHandler } from "./native/inputs.ts";
import { certifyForRow } from "./forCertify.ts";
import { checkAttrs, checkElement, forKeyField, rejectForKey, warnOnce } from "./attrs.ts";

export const INDENT = "    ";
const TEXT_TAGS = new Set(["text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

interface Props {
  classes: string[];
  classExpr?: t.Expression; // class={expr}: a dynamic class string (space-separated, like className)
  classList: Array<{ key: string; expr: t.Expression }>;
  onClick: t.Node | undefined;
  onMouseEnter?: t.Node;
  onMouseLeave?: t.Node;
  type?: string | undefined; // the `type` attr (e.g. <button type="submit"> → default button)
  ref: string | undefined; // the local var name a ref={ident} binds to
  // HTML5 drag-and-drop subset: draggable + dragData on the source, onDrop on the target.
  // The drop handler's parameter receives the SOURCE's dragData (not a DragEvent).
  draggable: boolean;
  dragData: t.Expression | undefined;
  onDrop: t.Node | undefined;
  title?: t.Expression; // the HTML `title` attribute → a hover tooltip (W.ToolTip)
  onScroll?: t.Node; // on an `overflow: auto` box: the engine's scrollChanged (see translateScrollHandler)
  onContextMenu?: t.Node; // right click: a RightButton MouseArea (see translateMouseHandler)
  onKeyDown?: t.Node; // keys on a focused box: Keys.onPressed (see translateKeyHandler)
  // Keyboard & accessibility (release-plan M5) — see docs/keyboard-accessibility.md.
  tabIndex?: number; // 0/positive → joins the Tab chain (activeFocusOnTab); -1 → programmatic only
  role?: string; // ARIA role → Accessible.role (ARIA_ROLE_MAP) — unknown roles warn, don't map
  ariaLabel?: t.Expression; // aria-label → Accessible.name (reactive)
  ariaDescription?: string; // aria-description/aria-describedby → Accessible.description (static text)
  ariaChecked?: t.Node; // aria-checked → Accessible.checked
  ariaPressed?: t.Node; // aria-pressed → Accessible.pressed
  ariaSelected?: t.Node; // aria-selected → Accessible.selected
  ariaHidden?: t.Node; // aria-hidden → Accessible.ignored
  ariaDisabled?: t.Node; // aria-disabled → enabled: !(…) (drives the standard disabled a11y state too)
}

/** Build the cssClass property line(s) for a native element.
 *  - No classes and no classList → empty (no line emitted)
 *  - Static classes only (no classList) → `cssClass: ["a", "b"]` (unchanged from before)
 *  - With classList → reactive concat expression:
 *    `cssClass: ["a"].concat(cond1 ? ["cls1"] : []).concat(cond2 ? ["cls2"] : [])`
 *  This keeps existing golden output byte-identical when classList is absent. */
export function buildCssClassLine(props: Props, scope: Scope, pad: string): string[] {
  const { classes, classList, classExpr } = props;
  if (classes.length === 0 && classList.length === 0 && !classExpr) return [];
  if (classList.length === 0 && !classExpr) {
    // Original static form — unchanged for goldens compatibility
    return [`${pad}cssClass: [${classes.map((c) => JSON.stringify(c)).join(", ")}]`];
  }
  // Reactive form: start with the static array (or the dynamic class string split like the DOM's
  // className), then chain .concat() for each classList entry
  const staticPart = classExpr
    ? `("" + ((${emitExpr(classExpr, { ...scope, mode: "binding" })}) ?? "")).split(/\\s+/).filter(Boolean)`
    : `[${classes.map((c) => JSON.stringify(c)).join(", ")}]`;
  const concats = classList.map(({ key, expr }) => {
    const cond = emitExpr(expr, { ...scope, mode: "binding" });
    return `.concat(${cond} ? [${JSON.stringify(key)}] : [])`;
  }).join("");
  return [`${pad}cssClass: ${staticPart}${concats}`];
}

/** Emit QML lines for a render `h(tag, props, ...children)` call. */
export function emitQml(call: t.CallExpression, scope: Scope, level = 0, guard?: string): string[] {
  const { tag: tagArg, props: propsArg, children } = hParts(call);
  // Fail loud: an attribute this element's emitter would not emit is a transpile error (attrs.ts).
  checkElement(call, TEXT_TAGS);

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
    if (tagArg.name === "VirtualList") return emitVirtualList(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Index") return emitIndex(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Switch") return emitSwitch(propsArg, children as t.Node[], scope, level, guard);
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
    // Pointer props on a text (`<text onClick>` — a link): the div's emission, the area a child of
    // the text (a CssText lays out no children; `anchors.fill` covers the run).
    const { refLine, stateLine, clickLines, tipLines } = emitInteractions(props, children as t.Node[], scope, level, false);
    return [
      `${pad}W.Text {`,
      ...classLine,
      ...stateLine,
      ...refLine,
      ...guardLine(guard, level),
      ...(tag !== "text" ? [`${pad}${INDENT}cssPrimitive: ${JSON.stringify(tag)}`] : []),
      `${pad}${INDENT}text: ${textBinding(children as t.Node[], scope)}`,
      ...clickLines,
      ...tipLines,
      `${pad}}`,
    ];
  }

  const { refLine, stateLine, clickLines, tipLines } = emitInteractions(props, children as t.Node[], scope, level, true);
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
    ...tipLines,
    `${pad}}`,
  ];
}

/** The pointer/keyboard props shared by every plain element (the web allows them on any element):
 *  ref, onClick (+hover), onKeyDown, onContextMenu, draggable/dragData, onDrop, title, and — on a
 *  box only (`scrollable`) — onScroll. Returns the lines the element splices into its own object;
 *  <div> and <text> use the SAME emission, so a click on a text behaves like a click on a div. */
function emitInteractions(props: Props, children: t.Node[], scope: Scope, level: number, scrollable: boolean) {
  const pad = INDENT.repeat(level);
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
  // A handler that takes the event (`(e) => … e.ctrlKey …`) reads the click's modifiers.
  const clickFn = props.onClick;
  const clickHandler = clickFn && (t.isArrowFunctionExpression(clickFn) || t.isFunctionExpression(clickFn)) && clickFn.params.length > 0
    ? `(mouse) => { ${translateMouseHandler(clickFn, scope)} }`
    : emitHandler(props.onClick, scope);
  const wantsHover = !!(props.onMouseEnter || props.onMouseLeave);
  // A child can own a hover area (including inside a component/conditional). MouseArea's
  // containsMouse then drops out. Observe the parent passively in that case, without bubbling.
  const passiveHover = wantsHover && (!clickHandler || hasHoverAreaDescendant(children));
  const hoverEdge = (condition: string, point: string) => {
    const handler = (node: t.Node | undefined) => {
      if (!node) return "";
      if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node))
        return translateArgsHandler(node, [point], scope);
      return emitHandler(node, scope) ?? "";
    };
    return `if (${condition}) { ${handler(props.onMouseEnter)} } else { ${handler(props.onMouseLeave)} }`;
  };
  let maId: string | null = null;
  if (clickHandler) {
    const counter = scope.hoverCounter ?? { n: 0 };
    maId = `__hover${counter.n++}`;
    stateLine.push(`${pad}${INDENT}cssState: ${maId}.containsMouse ? ["hover"] : []`);
    clickLines.push(
      `${pad}${INDENT}MouseArea {`,
      `${pad}${INDENT}${INDENT}id: ${maId}`,
      // Declared after the children, a filling area lies ON TOP of them and swallowed an inner
      // element's own click (a chip inside a clickable row never fired). With an interactive
      // descendant, the area goes beneath the content, so — as on the web — the innermost target
      // gets the click and plain content (text, fills) still passes it down to this one.
      ...(hasInteractiveDescendant(children) ? [`${pad}${INDENT}${INDENT}z: -1`] : []),
      `${pad}${INDENT}${INDENT}anchors.fill: parent`,
      `${pad}${INDENT}${INDENT}hoverEnabled: true`,
      `${pad}${INDENT}${INDENT}cursorShape: Qt.PointingHandCursor`,
      `${pad}${INDENT}${INDENT}onClicked: ${clickHandler}`,
      ...(wantsHover && !passiveHover ? [`${pad}${INDENT}${INDENT}onContainsMouseChanged: { ${hoverEdge("containsMouse", "({offsetX: mouseX, offsetY: mouseY})")} }`] : []),
      `${pad}${INDENT}}`,
    );
  }
  // onKeyDown on a box (a list taking the keyboard): Keys.onPressed while it has the focus.
  // Merged with role-based Enter/Space ACTIVATION below — a QML object can only have one
  // Keys.onPressed binding, so both land in the same handler instead of fighting over it.
  const keyPressStmts: string[] = [];
  if (props.onKeyDown) {
    const fn = props.onKeyDown;
    if (!(t.isArrowFunctionExpression(fn) || t.isFunctionExpression(fn)))
      throw new Error("onKeyDown must be an inline function: (e) => …");
    keyPressStmts.push(translateKeyHandler(fn, scope));
  }
  // WAI-ARIA keyboard practice: role="button"/"link"/"menuitem"/"tab" (+onClick) activates on
  // Enter AND Space — Space on key RELEASE for "button" (the browser contract: it also swallows
  // Space on press, so the page doesn't scroll first); role="checkbox"/"switch" toggles on Space
  // alone. Only while the element has the focus (Keys delivers nothing otherwise).
  let keyReleaseStmt: string | null = null;
  if (props.role && clickHandler) {
    // The same statement the MouseArea's onClicked runs: a function value (mouse param) must be
    // CALLED, a bare expression (no param) already IS the call/body — reuse it either way.
    const activate = clickFn && (t.isArrowFunctionExpression(clickFn) || t.isFunctionExpression(clickFn)) && clickFn.params.length > 0
      ? `(${clickHandler})(event)` : clickHandler;
    const onEnterSpace = new Set(["button", "link", "menuitem", "tab"]);
    const onSpaceOnly = new Set(["checkbox", "switch"]);
    if (onEnterSpace.has(props.role)) {
      keyPressStmts.push(`if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { ${activate}; event.accepted = true; }`);
      if (props.role === "button") {
        keyPressStmts.push(`if (event.key === Qt.Key_Space) { event.accepted = true; }`);
        keyReleaseStmt = `if (event.key === Qt.Key_Space) { ${activate}; event.accepted = true; }`;
      } else {
        keyPressStmts.push(`if (event.key === Qt.Key_Space) { ${activate}; event.accepted = true; }`);
      }
    } else if (onSpaceOnly.has(props.role)) {
      keyPressStmts.push(`if (event.key === Qt.Key_Space) { ${activate}; event.accepted = true; }`);
    }
  }
  if (keyPressStmts.length) stateLine.push(`${pad}${INDENT}Keys.onPressed: (event) => { ${keyPressStmts.join(" ")} }`);
  if (keyReleaseStmt) stateLine.push(`${pad}${INDENT}Keys.onReleased: (event) => { ${keyReleaseStmt} }`);
  // tabIndex / role / aria-* (M5): Accessible.* attached properties + the tab-focus chain — see
  // docs/keyboard-accessibility.md for the full table. A plain element with none of these gets NO
  // extra line (unchanged output), matching "generic containers get no role" (a11y.cpp).
  if (props.tabIndex !== undefined) {
    if (props.tabIndex > 0) {
      warnOnce(`tabIndex-positive:${props.tabIndex}`,
        `[solid-qml] tabIndex={${props.tabIndex}}: positive values are treated like 0 (DOM document order; no priority reordering)`);
    }
    if (props.tabIndex >= 0) stateLine.push(`${pad}${INDENT}activeFocusOnTab: ${TABSTOP_GUARD}`);
    // tabIndex={-1}: no line — activeFocusOnTab defaults to false (not in the Tab chain), and
    // ref.forceActiveFocus() already works on ANY item regardless (QQuickItem, not our own API).
  }
  if (props.role) {
    const mapped = ARIA_ROLE_MAP[props.role];
    if (mapped) stateLine.push(`${pad}${INDENT}Accessible.role: Accessible.${mapped}`);
    else warnOnce(`role:${props.role}`, `[solid-qml] role="${props.role}": unknown ARIA role, no native mapping (ignored)`);
  }
  if (props.ariaLabel) stateLine.push(`${pad}${INDENT}Accessible.name: ${emitExpr(props.ariaLabel, { ...scope, mode: "binding" })}`);
  if (props.ariaDescription !== undefined) stateLine.push(`${pad}${INDENT}Accessible.description: ${JSON.stringify(props.ariaDescription)}`);
  if (props.ariaChecked) stateLine.push(`${pad}${INDENT}Accessible.checked: ${ariaBoolExpr(props.ariaChecked, scope)}`);
  if (props.ariaPressed) stateLine.push(`${pad}${INDENT}Accessible.pressed: ${ariaBoolExpr(props.ariaPressed, scope)}`);
  if (props.ariaSelected) stateLine.push(`${pad}${INDENT}Accessible.selected: ${ariaBoolExpr(props.ariaSelected, scope)}`);
  if (props.ariaHidden) stateLine.push(`${pad}${INDENT}Accessible.ignored: ${ariaBoolExpr(props.ariaHidden, scope)}`);
  if (props.ariaDisabled) stateLine.push(`${pad}${INDENT}enabled: !(${ariaBoolExpr(props.ariaDisabled, scope)})`);
  // onContextMenu: a right click — its own RightButton area (left clicks, hover and wheel pass), the
  // handler's e.clientX / e.clientY in scene coordinates (what <Menu ref={r}>'s r.open(x, y) takes).
  if (props.onContextMenu) {
    const fn = props.onContextMenu;
    if (!(t.isArrowFunctionExpression(fn) || t.isFunctionExpression(fn)))
      throw new Error("onContextMenu must be an inline function: (e) => …");
    clickLines.push(
      `${pad}${INDENT}MouseArea {`,
      ...(hasInteractiveDescendant(children) ? [`${pad}${INDENT}${INDENT}z: -1`] : []),
      `${pad}${INDENT}${INDENT}anchors.fill: parent`,
      `${pad}${INDENT}${INDENT}acceptedButtons: Qt.RightButton`,
      `${pad}${INDENT}${INDENT}onClicked: (mouse) => { ${translateMouseHandler(fn, scope)} }`,
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
  // onScroll: the box's own scrollChanged (CssRect scrollTop/scrollHeight/clientHeight, the DOM's
  // names) — a long list windows on it, loading more rows near the end.
  if (props.onScroll) {
    if (!scrollable) throw new Error("onScroll: only a box (<div>) scrolls; a text has no scroll position");
    const fn = props.onScroll;
    if (!(t.isArrowFunctionExpression(fn) || t.isFunctionExpression(fn)))
      throw new Error("onScroll must be an inline function: (e) => …");
    stateLine.push(`${pad}${INDENT}onScrollChanged: { ${translateScrollHandler(fn, scope)} }`);
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
  // title="…" → a hover tooltip (W.ToolTip: Templates behaviour, CSS look). It follows the click
  // area's hover when there is one; otherwise a passive HoverHandler, which takes no clicks.
  const tipLines: string[] = [];
  let passiveId: string | null = null;
  if (passiveHover) {
    const counter = scope.hoverCounter ?? { n: 0 };
    passiveId = `__enter${counter.n++}`;
    tipLines.push(`${pad}${INDENT}HoverHandler {`,
      `${pad}${INDENT}${INDENT}id: ${passiveId}`,
      `${pad}${INDENT}${INDENT}onHoveredChanged: { ${hoverEdge("hovered", "({offsetX: point.position.x, offsetY: point.position.y})")} }`,
      `${pad}${INDENT}}`);
  }
  if (props.title) {
    let hover = passiveId ? `${passiveId}.hovered` : maId ? `${maId}.containsMouse` : "";
    if (!hover) {
      const counter = scope.hoverCounter ?? { n: 0 };
      const hid = `__tip${counter.n++}`;
      tipLines.push(`${pad}${INDENT}HoverHandler {`, `${pad}${INDENT}${INDENT}id: ${hid}`, `${pad}${INDENT}}`);
      hover = `${hid}.hovered`;
    }
    tipLines.push(
      `${pad}${INDENT}W.ToolTip {`,
      `${pad}${INDENT}${INDENT}text: ${emitExpr(props.title, { ...scope, mode: "binding" })}`,
      `${pad}${INDENT}${INDENT}shown: ${hover}`,
      `${pad}${INDENT}}`,
    );
  }
  return { refLine, stateLine, clickLines, tipLines };
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
    // ref={x} binds the FOREIGN instance itself (not the wrapper box) — handler calls like
    // `term.copySelection()` land on the component.
    const refLine: string[] = [];
    if (props.ref) {
      refLine.push(`${i(2)}id: _ref_${safeName(props.ref)}`);
      if (scope.refs) scope.refs.push(props.ref);
    }
    const inner = [`${i(1)}${typeName} {`, ...refLine, `${i(2)}anchors.fill: parent`];
    if (propsArg && t.isObjectExpression(propsArg)) {
      for (const p of propsArg.properties) {
        if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
        const k = p.key.name;
        if (k === "children" || k === "class" || k === "classList" || k === "ref") continue;
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
    // No focus-ring overlay: each control paints its OWN ring from the shared reset's
    // `:focus { outline }` (native by default — the ring moves with the control and needs no
    // window-level tracker). Apps restyle it by overriding `:focus`.
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
    ...emitAriaForNativeControl(props, scope, i(1)),
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
  // Resolve the `src` prop via emitExpr in binding mode; `alt` becomes the ACCESSIBLE NAME
  // (SolidWidgets::Image maps it to the Graphic role's name — HTML's contract, and an image with
  // no alt stays unnamed, i.e. decorative, which AT skips).
  let src = '""';
  let alt: string | null = null;
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
      if (p.key.name === "src") src = emitExpr(p.value, { ...scope, mode: "binding" });
      else if (p.key.name === "alt") alt = emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }
  return [
    `${pad}W.Image {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}src: ${src} || ""`,
    ...(alt !== null ? [`${i(1)}alt: ${alt}`] : []),
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
    // Block-bodied handler: every statement (an `if` too — a whole kind of statement was dropped).
    body = fn.body.body.map((s) => emitStmt(s, inner)).join(" ");
  } else {
    body = emitExpr(fn.body, inner);
  }
  return body
    .replace(/__ev\.(?:currentTarget|target)\.value/g, "text")
    .replace(/__ev\.key\s*===\s*"Enter"/g, "(event.key === Qt.Key_Return || event.key === Qt.Key_Enter)")
    .replace(/__ev\.key\s*===\s*"Escape"/g, "(event.key === Qt.Key_Escape)")
    .replace(/__ev\.key/g, "event.key");
}

/** DOM key names (KeyboardEvent.key) → Qt keys. */
const DOM_KEYS: Record<string, string> = {
  ArrowDown: "Qt.Key_Down", ArrowUp: "Qt.Key_Up", ArrowLeft: "Qt.Key_Left", ArrowRight: "Qt.Key_Right",
  Home: "Qt.Key_Home", End: "Qt.Key_End", PageUp: "Qt.Key_PageUp", PageDown: "Qt.Key_PageDown",
  Delete: "Qt.Key_Delete", Backspace: "Qt.Key_Backspace", Escape: "Qt.Key_Escape", Tab: "Qt.Key_Tab", " ": "Qt.Key_Space",
};

/** A key handler on a box: `e.key === "ArrowDown"` (any DOM key name, or one letter/digit) compares
 *  event.key; e.shiftKey / ctrlKey / altKey / metaKey read event.modifiers; e.preventDefault()
 *  accepts the key (else it goes on to the box's parents, as on the web). Runs inside
 *  `(event) => { … }`. */
function translateKeyHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  const body = t.isBlockStatement(fn.body)
    ? fn.body.body.map((s) => emitStmt(s, inner)).join(" ")
    : `${emitExpr(fn.body, inner)};`;
  const MOD: Record<string, string> = { ctrlKey: "ControlModifier", shiftKey: "ShiftModifier", altKey: "AltModifier", metaKey: "MetaModifier" };
  const keyOf = (name: string): string => {
    if (name === "Enter") return "(event.key === Qt.Key_Return || event.key === Qt.Key_Enter)";
    if (DOM_KEYS[name]) return `(event.key === ${DOM_KEYS[name]})`;
    if (/^[a-zA-Z0-9]$/.test(name)) return `(event.key === Qt.Key_${name.toUpperCase()})`;
    throw new Error(`onKeyDown: key "${name}" is not mapped`);
  };
  const out = body
    .replace(/__ev\.(?:preventDefault)\(\);?/g, "event.accepted = true;")
    .replace(/__ev\.(?:stopPropagation)\(\);?/g, "")
    .replace(/__ev\.key\s*===\s*"([^"]*)"/g, (_m, k: string) => keyOf(k))
    .replace(/__ev\.key\s*!==\s*"([^"]*)"/g, (_m, k: string) => `!${keyOf(k)}`)
    .replace(/__ev\.(ctrlKey|shiftKey|altKey|metaKey)\b/g, (_m, k: string) => `((event.modifiers & Qt.${MOD[k]}) !== 0)`);
  // The event itself is used (handed to a helper): a DOM-shaped object stands in for it.
  if (/__ev\b/.test(out)) return `var __ev = ${domKeyEvent()}; ${out}`;
  return out;
}

/** A KeyboardEvent-shaped object over a QML KeyEvent `event`: key (DOM name; a printable key as
 *  its character, upper-cased with Shift), the modifier flags, preventDefault() accepting it. */
function domKeyEvent(): string {
  const names = Object.entries(DOM_KEYS).map(([dom, qt]) => `event.key === ${qt} ? ${JSON.stringify(dom)}`).join(" : ");
  const key = `(event.key === Qt.Key_Return || event.key === Qt.Key_Enter ? "Enter" : ${names} : `
    + `(event.key >= 0x21 && event.key <= 0x7e ? ((event.modifiers & Qt.ShiftModifier) ? String.fromCharCode(event.key) : String.fromCharCode(event.key).toLowerCase()) : event.text))`;
  return `{ key: ${key}, shiftKey: (event.modifiers & Qt.ShiftModifier) !== 0, ctrlKey: (event.modifiers & Qt.ControlModifier) !== 0, `
    + `altKey: (event.modifiers & Qt.AltModifier) !== 0, metaKey: (event.modifiers & Qt.MetaModifier) !== 0, `
    + `preventDefault: function() { event.accepted = true }, stopPropagation: function() {} }`;
}

/** A mouse handler taking the event: `e.ctrlKey` / `shiftKey` / `altKey` / `metaKey` read the
 *  click's modifiers, `e.clientX` / `e.clientY` its scene position; preventDefault() and
 *  stopPropagation() are no-ops (the area accepts the click). Runs inside `(mouse) => { … }`. */
function translateMouseHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  const body = t.isBlockStatement(fn.body)
    ? fn.body.body.map((s) => emitStmt(s, inner)).join(" ")
    : `${emitExpr(fn.body, inner)};`;
  const MOD: Record<string, string> = { ctrlKey: "ControlModifier", shiftKey: "ShiftModifier", altKey: "AltModifier", metaKey: "MetaModifier" };
  let out = body
    .replace(/__ev\.(?:preventDefault|stopPropagation)\(\);?/g, "")
    .replace(/__ev\.(ctrlKey|shiftKey|altKey|metaKey)\b/g, (_m, k: string) => `((mouse.modifiers & Qt.${MOD[k]}) !== 0)`)
    .replace(/__ev\.clientX\b/g, "__p.x")
    .replace(/__ev\.clientY\b/g, "__p.y");
  if (/__ev\b/.test(out))
    throw new Error("a mouse handler reads only e.ctrlKey/shiftKey/altKey/metaKey, e.clientX/clientY, e.preventDefault()");
  if (/__p\./.test(out)) out = `var __p = mapToItem(null, mouse.x, mouse.y); ${out}`;
  return out;
}

/** An onScroll handler: `e.currentTarget.scrollTop` / `scrollHeight` / `clientHeight` (or
 *  `e.target.…`) are the box's OWN properties — bare names, which an element's handler resolves
 *  against the element first. */
function translateScrollHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = { ...scope, mode: "handler", locals: { ...(scope.locals ?? {}), ...(paramName ? { [paramName]: "__ev" } : {}) } };
  const body = t.isBlockStatement(fn.body)
    ? fn.body.body.map((s) => emitStmt(s, inner)).join(" ")
    : `${emitExpr(fn.body, inner)};`;
  const out = body.replace(/__ev\.(?:currentTarget|target)\.(scrollTop|scrollHeight|clientHeight)\b/g, "$1");
  if (/__ev\b/.test(out)) throw new Error("onScroll: only e.currentTarget.scrollTop / scrollHeight / clientHeight are supported");
  return out;
}

/** A boolean HTML attribute (`disabled`, `readOnly`) as a QML expression for "it holds": a bare or
 *  literal `true` attribute → "true", literal `false` → null (absent), any expression → a binding. */
export function boolAttrExpr(value: t.Node, scope: Scope): string | null {
  if (t.isBooleanLiteral(value)) return value.value ? "true" : null;
  if (!t.isExpression(value)) return "true";
  return emitExpr(value, { ...scope, mode: "binding" });
}
/** `enabled:` value for a `disabled` expression ("true" → false, as before). */
export function enabledFromDisabled(disabled: string): string {
  return disabled === "true" ? "false" : `!(${disabled})`;
}

/** Parse all widget-relevant attrs from a propsArg ObjectExpression into a plain record.
 *  Returns the typed, name-normalised set of values the widget emitters need. */
function readWidgetProps(propsArg: t.Node | undefined, scope: Scope): {
  type: string; valueExpr: string | null; signalName: string | null;
  placeholder: string | null; // a QML expression (a quoted literal, or a binding), null = none
  onInputFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  onKeyDownFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null;
  disabled: string | null; readOnly: string | null; maxLength: string | null;
  // Phase 3: toggle/radio attrs
  role: string; name: string | null; checkedExpr: string | null;
  // Phase 4: range/number attrs
  min: string; max: string; step: string;
} {
  let type = "text";
  let valueExpr: string | null = null;
  let signalName: string | null = null;
  let placeholder: string | null = null;
  let onInputFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let onKeyDownFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let disabled: string | null = null;
  let readOnly: string | null = null;
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
      // A literal stays a literal (an empty one is no placeholder); an expression is a binding.
      if (key === "placeholder" && t.isStringLiteral(p.value)) placeholder = p.value.value ? JSON.stringify(p.value.value) : null;
      else if (key === "placeholder" && t.isExpression(p.value)) placeholder = emitExpr(p.value, { ...scope, mode: "binding" });
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
      if (key === "disabled") disabled = boolAttrExpr(p.value, scope);
      if (key === "readonly" || key === "readOnly") readOnly = boolAttrExpr(p.value, scope);
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
  // A ref={x} names the control itself (its id IS the ref), so `x.forceActiveFocus()` reaches it.
  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = props.ref ? `_ref_${safeName(props.ref)}` : `__input${counter.n++}`;
  if (props.ref && scope.refs) scope.refs.push(props.ref);

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
  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  if (readOnly !== null) lines.push(`${i(1)}readOnly: ${readOnly}`);
  if (maxLength !== null) lines.push(`${i(1)}maximumLength: ${maxLength}`);
  if (placeholder !== null) lines.push(`${i(1)}placeholder: ${placeholder}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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
  const ctlId = props.ref ? `_ref_${safeName(props.ref)}` : `__input${counter.n++}`;
  if (props.ref && scope.refs) scope.refs.push(props.ref);

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

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  if (readOnly !== null) lines.push(`${i(1)}readOnly: ${readOnly}`);
  if (placeholder !== null) lines.push(`${i(1)}placeholder: ${placeholder}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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
  // aria-label/aria-description: no C++ wrapper here (the radio is a raw T.RadioButton, unlike
  // the other inputs) — set Accessible.name/.description directly on the control itself.
  lines.push(...emitAriaForNativeControl(props, scope, i(2), { name: "Accessible.name", description: "Accessible.description" }));

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

  if (disabled !== null) lines.push(`${i(2)}enabled: ${enabledFromDisabled(disabled)}`);
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
  // Replace the DOM-style accessor with the QML equivalent expression, then any BARE `__ev`
  // (a handler that uses the value param directly, `(v) => setX(v)`, not `v.target.value`) —
  // otherwise the param stays undefined and the write silently no-ops (settings not saved).
  return body
    .replace(/__ev\.(?:currentTarget|target)\.value/g, valueExpr)
    .replace(/\b__ev\b/g, valueExpr);
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

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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

/** A tag's name for an error message — "div", "For", or "?" for anything else. A plain helper (not
 *  inlined) so TS's control-flow narrowing from an earlier `isIdentifier(tag, {name: …})` match
 *  check elsewhere in the caller can't leak in and narrow this away to `never`. */
function describeTag(tag: t.Node): string {
  if (t.isStringLiteral(tag)) return tag.value;
  if (t.isIdentifier(tag)) return tag.name;
  return "?";
}

/** Static option entry parsed from a <option value="v">Label</option> child of <select>. */
interface OptionEntry { label: string; value: string }

/** Parse one static <option value="v">Label</option> element into {label, value}. Throws a clear
 *  transpiler error on dynamic content (interpolations, sub-elements, computed values) — shared by
 *  the fully-static <select> path and a static run inside a dynamic (<For>-bearing) one. */
function parseStaticOptionEntry(optProps: t.Node | undefined, optChildren: t.Node[]): OptionEntry {
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
  return { label, value };
}

/** Parse the <option> children of a <select> into a list of {label, value} pairs. Used ONLY for the
 *  fully-static <select> (no <For>, no options={} attr) — kept byte-identical to the original Phase
 *  4 plan so every existing <select> (and the gallery regen) is untouched by this plan. */
function parseOptions(children: t.Node[]): OptionEntry[] {
  const entries: OptionEntry[] = [];
  for (const child of children) {
    if (!isHCall(child)) continue; // skip whitespace JSXText between options
    const { tag, props: optProps, children: optChildren } = hParts(child as t.CallExpression);
    if (!t.isStringLiteral(tag) || tag.value !== "option")
      throw new Error(`<select> only accepts <option> children, got <${t.isStringLiteral(tag) ? tag.value : "?"}>`);
    entries.push(parseStaticOptionEntry(optProps, optChildren as t.Node[]));
  }
  return entries;
}

/** One segment of a DYNAMIC <select>'s model: three PARALLEL QML-JS array expressions of the same
 *  length/order — labels (→ model), values (→ values) and a disabled flag per row (→
 *  disabledFlags). Segments concatenate in source order, so static <option>s before/after a <For>
 *  interleave exactly like the DOM would render them (plan item 2, "mixing"). */
interface SelectSegment { model: string; values: string; disabled: string }

function staticOptionSegment(entries: OptionEntry[]): SelectSegment {
  return {
    model: `[${entries.map((o) => JSON.stringify(o.label)).join(", ")}]`,
    values: `[${entries.map((o) => JSON.stringify(o.value)).join(", ")}]`,
    disabled: `[${entries.map(() => "false").join(", ")}]`,
  };
}

/** <select>'s <For each={E}>{(item) => <option value=…>…</option>}</For> child (plan item 1) — one
 *  row's worth of QML-JS `.map()` expressions over the For's reactive list. There is no QML
 *  Repeater/delegate here (the model is a flat array the T.ComboBox binds to directly): `item` is
 *  shadowed into scope exactly like a real .map() callback parameter (shadowOwnParams), and the
 *  option's children/value/disabled are emitted against that scope. Mirrors emitFor's single-param
 *  delegate convention — no index param (not supported for <For> elsewhere in this plan either). */
function parseForOptionSegment(forPropsArg: t.Node | undefined, forChildren: t.Node[], scope: Scope): SelectSegment {
  checkAttrs("For", forPropsArg);
  rejectForKey(forPropsArg, "<select>");
  const each = readEach(forPropsArg, scope);
  const delegate = forChildren.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as
    t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (!delegate) throw new Error("<select><For> needs a row function: {(item) => <option value={…}>…</option>}");
  if (delegate.params.length > 1)
    throw new Error("<select><For> row function takes a single parameter (item) in this plan — no index param");
  const param = delegate.params[0];
  const itemName = param && t.isIdentifier(param) ? param.name : null;
  const body = delegate.body;
  if (!isHCall(body))
    throw new Error("<select><For> row function must return a single <option> element");
  const { tag: bodyTag, props: bodyProps, children: bodyChildren } = hParts(body as t.CallExpression);
  if (!t.isStringLiteral(bodyTag) || bodyTag.value !== "option")
    throw new Error(`<select><For> row must be a single <option>, got <${describeTag(bodyTag)}>`);

  const inner: Scope = param ? shadowOwnParams(scope, [param]) : scope;
  const itemSafe = itemName ? safeName(itemName) : "_";

  // Label: the option's own children — strings/JSXText concatenate, interpolations emit as
  // bindings (unlike the static path, dynamic content is the whole point here).
  const labelParts: string[] = [];
  for (const c of bodyChildren as t.Node[]) {
    if (t.isStringLiteral(c)) { if (c.value.trim() || /\s/.test(c.value)) labelParts.push(JSON.stringify(c.value)); }
    else if (isJsxText(c)) { const cleaned = (c as any).value.replace(/\s+/g, " "); if (cleaned.trim()) labelParts.push(JSON.stringify(cleaned)); }
    else if (t.isExpression(c)) labelParts.push(`(${emitExpr(c, { ...inner, mode: "binding" })})`);
    else throw new Error("<select><For> <option> label: unsupported child");
  }
  const labelExpr = labelParts.length === 0 ? `""` : labelParts.join(" + ");

  let valueExpr = labelExpr; // defaults to the label, same convention as a static <option>
  let disabledExpr = "false";
  if (bodyProps && t.isObjectExpression(bodyProps)) {
    for (const p of (bodyProps as t.ObjectExpression).properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key))
        throw new Error("<select><For> <option>: unsupported prop");
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value)) { valueExpr = emitExpr(p.value, { ...inner, mode: "binding" }); continue; }
      if (key === "disabled") { disabledExpr = boolAttrExpr(p.value, inner) ?? "false"; continue; }
      throw new Error(`<select><For> <option> does not support \`${key}\` — only value/disabled and its text children`);
    }
  }

  return {
    // labelExpr/valueExpr/disabledExpr already carry their own grouping (built the same way
    // textBinding does for a plain text run) — no extra outer parens here.
    model: `(${each}).map((${itemSafe}) => ${labelExpr})`,
    values: `(${each}).map((${itemSafe}) => ${valueExpr})`,
    disabled: `(${each}).map((${itemSafe}) => ${disabledExpr})`,
  };
}

/** `options={expr}` (plan item 3): an array of plain label strings OR {value, label, disabled?}
 *  objects — <select> sugar for a runtime option list without writing a <For> by hand. Polymorphic
 *  at runtime (`typeof` check) since the array may mix either shape. */
function optionsAttrSegment(expr: string): SelectSegment {
  return {
    model: `(${expr}).map((o) => (typeof o === "string" ? o : o.label))`,
    values: `(${expr}).map((o) => (typeof o === "string" ? o : (("value" in o) ? o.value : o.label)))`,
    disabled: `(${expr}).map((o) => (typeof o === "string" ? false : !!o.disabled))`,
  };
}

function concatSegments(segs: SelectSegment[], field: "model" | "values" | "disabled"): string {
  return segs.map((s) => s[field]).reduce((a, b) => `${a}.concat(${b})`);
}

/** Parse <select>'s children into a sequence of SelectSegments: consecutive static <option>s batch
 *  into one literal-array segment (so a fully-static run reads exactly like the Phase 4 output), a
 *  <For> becomes a .map() segment over its list, and anything else — <optgroup>, any other tag — is
 *  a fail-loud transpile error (no silent drop; plan's explicit unsupported-cases requirement). */
function parseDynamicSelectChildren(children: t.Node[], scope: Scope): SelectSegment[] {
  const segs: SelectSegment[] = [];
  let run: OptionEntry[] = [];
  const flush = () => { if (run.length) { segs.push(staticOptionSegment(run)); run = []; } };
  for (const child of children) {
    if (!isHCall(child)) continue; // skip whitespace JSXText between children
    const { tag, props: elProps, children: elChildren } = hParts(child as t.CallExpression);
    if (t.isIdentifier(tag, { name: "For" })) {
      flush();
      segs.push(parseForOptionSegment(elProps, elChildren as t.Node[], scope));
      continue;
    }
    if (t.isStringLiteral(tag) && tag.value === "option") {
      run.push(parseStaticOptionEntry(elProps, elChildren as t.Node[]));
      continue;
    }
    if (t.isStringLiteral(tag) && tag.value === "optgroup")
      throw new Error("<select>: <optgroup> is not supported yet — flatten its <option>s as direct children");
    throw new Error(`<select> only accepts <option> or <For> children, got <${describeTag(tag)}>`);
  }
  flush();
  return segs;
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
 *  onChange → onActivated (__idx) with synthetic value __input<n>.__values[__idx] (NOT `index` —
 *  see rowDelegate's `indexRef` comment in this file: a <For> row's own `i()` is routed through a
 *  reserved `__forIndex` property precisely so no synthetic handler parameter can shadow it).
 *
 *  DYNAMIC options (this plan): a <For> child, static <option>s mixed before/after it, and/or an
 *  `options={expr}` attribute all build the SAME `model`/`values` (now QML-JS expressions rather
 *  than literal arrays) plus a `disabledFlags` parallel array — see parseDynamicSelectChildren /
 *  optionsAttrSegment. A <select> with NEITHER keeps the ORIGINAL literal-array emission verbatim
 *  (no textual or behavioural change). Anything still unsupported (<optgroup>, a <For> row that
 *  isn't a single <option>, any other child tag) is a fail-loud transpile error. */
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

  // Read value/onChange/disabled/options from the <select> propsArg directly.
  let valueExpr: string | null = null;
  let onChangeFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let disabled: string | null = null;
  let optionsExpr: string | null = null;
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value))
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (key === "onChange" && t.isExpression(p.value)
          && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "disabled") disabled = boolAttrExpr(p.value, scope);
      if (key === "options" && t.isExpression(p.value)) optionsExpr = emitExpr(p.value, { ...scope, mode: "binding" });
    }
  }

  // A <For> child (anywhere among the children) or an `options={}` attribute makes the option list
  // DYNAMIC — it must follow a reactive list at runtime. Pure static <option>s (the original Phase
  // 4 case) take the untouched literal-array path below.
  const hasFor = (children as t.Node[]).some((c) => {
    if (!isHCall(c)) return false;
    const { tag } = hParts(c as t.CallExpression);
    return t.isIdentifier(tag, { name: "For" });
  });
  const dynamic = hasFor || optionsExpr !== null;

  let modelArr: string;
  let valuesArr: string;
  let disabledArr: string | null = null;

  if (!dynamic) {
    // Original Phase 4 path — byte-identical emission for every pre-existing <select>.
    const options = parseOptions(children);
    modelArr = `[${options.map((o) => JSON.stringify(o.label)).join(", ")}]`;
    valuesArr = `[${options.map((o) => JSON.stringify(o.value)).join(", ")}]`;
  } else {
    const segs: SelectSegment[] = [];
    if (optionsExpr !== null) segs.push(optionsAttrSegment(optionsExpr));
    segs.push(...parseDynamicSelectChildren(children, scope));
    if (segs.length === 0) throw new Error("<select>: no options — pass options={…} or <option>/<For> children");
    modelArr = concatSegments(segs, "model");
    valuesArr = concatSegments(segs, "values");
    disabledArr = concatSegments(segs, "disabled");
  }

  // onActivated(__idx): translate onChange so e.target.value → the picked value. `${ctlId}.values`
  // resolves against the component's `values` array on the instance. The synthetic param is named
  // `__idx`, NOT `index` — this handler may sit inside a <For> row whose own `i()` is routed
  // through a reserved `__forIndex` property (qml.ts's rowDelegate) precisely so that no such
  // synthetic parameter can shadow it; naming this one `index` too would defeat that on sight.
  const onActivatedBody = onChangeFn
    ? translateValueHandler(onChangeFn, `${ctlId}.values[__idx]`, scope)
    : "";

  const lines: string[] = [
    `${pad}W.Select {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}model: ${modelArr}`,
    `${i(1)}values: ${valuesArr}`,
    ...(disabledArr !== null ? [`${i(1)}disabledFlags: ${disabledArr}`] : []),
  ];

  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
  if (onActivatedBody) lines.push(`${i(1)}onActivated: (__idx) => { ${onActivatedBody} }`);

  // Binding: keep currentIndex in sync with the controlled value expression. `values.indexOf(...)`
  // already tracks the SAME value if the (possibly reordered) list still has it — the Binding is
  // reactive on `${ctlId}.values`. A dynamic list additionally clamps -1 (value no longer present)
  // to 0 — web <select> semantics (falls back to the first option) — left untouched for the static
  // path (no behavioural change to any <select> written before this plan).
  if (valueExpr !== null) {
    const indexValue = dynamic
      ? `Math.max(0, ${ctlId}.values.indexOf(${valueExpr}))`
      : `${ctlId}.values.indexOf(${valueExpr})`;
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${ctlId}`,
      `${i(2)}property: "currentIndex"`,
      `${i(2)}value: ${indexValue}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
    // Swapping `values` for a NEW array (the list signal firing) makes the underlying T.ComboBox
    // reset currentIndex to 0 *after* the Binding above already recomputed the right index
    // (QtQuick's own model-reset handling settles on a LATER event-loop pass than the Binding —
    // verified empirically with a headless probe: the Binding's value is already correct by the
    // time Qt.callLater runs, but currentIndex has since been clobbered back to 0, and nothing
    // re-fires the Binding until `${valueExpr}` itself changes again). Re-push the same expression
    // once more, deferred, so it wins the LAST write — the only way to survive a reset that lands
    // strictly after a live QML Binding with no earlier hook to intercept it.
    if (dynamic) lines.push(`${i(1)}onValuesChanged: Qt.callLater(function() { ${ctlId}.currentIndex = ${indexValue} })`);
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
  let disabled: string | null = null;

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value))
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
      if (key === "onChange" && t.isExpression(p.value)
        && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onChangeFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "disabled") disabled = boolAttrExpr(p.value, scope);
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
  if (disabled !== null) lines.push(`${i(1)}enabled: ${enabledFromDisabled(disabled)}`);
  lines.push(...emitAriaForNativeControl(props, scope, i(1)));
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

export function readProps(propsArg: t.Node | undefined): Props {
  const props: Props = { classes: [], classList: [], onClick: undefined, type: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
  if (!propsArg || !t.isObjectExpression(propsArg)) return props;
  for (const p of propsArg.properties) {
    // `aria-*` keys are string literals (the hyphen is not a valid identifier char) — read both forms.
    if (!t.isObjectProperty(p)) continue;
    const key = t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? p.key.value : null;
    if (key === null) continue;
    if (key === "class" && t.isStringLiteral(p.value)) props.classes = p.value.value.split(/\s+/).filter(Boolean);
    else if (key === "class" && t.isExpression(p.value)) props.classExpr = p.value;
    if (key === "classList" && t.isObjectExpression(p.value)) {
      for (const cp of p.value.properties) {
        if (!t.isObjectProperty(cp)) continue;
        const ck = t.isIdentifier(cp.key) ? cp.key.name : t.isStringLiteral(cp.key) ? cp.key.value : null;
        if (ck && t.isExpression(cp.value)) props.classList.push({ key: ck, expr: cp.value });
      }
    }
    if (key === "onClick") props.onClick = p.value;
    if (key === "onMouseEnter") props.onMouseEnter = p.value;
    if (key === "onMouseLeave") props.onMouseLeave = p.value;
    if (key === "type" && t.isStringLiteral(p.value)) props.type = p.value.value;
    if (key === "draggable") props.draggable = !t.isBooleanLiteral(p.value) || p.value.value;
    if (key === "dragData" && t.isExpression(p.value)) props.dragData = p.value;
    if (key === "onDrop") props.onDrop = p.value;
    if (key === "ref" && t.isIdentifier(p.value)) props.ref = p.value.name;
    if (key === "title" && t.isExpression(p.value)) props.title = p.value;
    if (key === "onScroll") props.onScroll = p.value;
    if (key === "onContextMenu") props.onContextMenu = p.value;
    if (key === "onKeyDown") props.onKeyDown = p.value;
    // Keyboard & accessibility (M5).
    if (key === "tabIndex" || key === "tabindex") {
      const n = t.isUnaryExpression(p.value) && p.value.operator === "-" ? p.value.argument : p.value;
      if (t.isNumericLiteral(n)) props.tabIndex = t.isUnaryExpression(p.value) ? -n.value : n.value;
    }
    if (key === "role" && t.isStringLiteral(p.value)) props.role = p.value.value;
    if (key === "aria-label" && t.isExpression(p.value)) props.ariaLabel = p.value;
    if ((key === "aria-description" || key === "aria-describedby") && t.isStringLiteral(p.value)) props.ariaDescription = p.value.value;
    if (key === "aria-checked" && t.isExpression(p.value)) props.ariaChecked = p.value;
    if (key === "aria-pressed" && t.isExpression(p.value)) props.ariaPressed = p.value;
    if (key === "aria-selected" && t.isExpression(p.value)) props.ariaSelected = p.value;
    if (key === "aria-hidden" && t.isExpression(p.value)) props.ariaHidden = p.value;
    if (key === "aria-disabled" && t.isExpression(p.value)) props.ariaDisabled = p.value;
  }
  return props;
}

/** aria-label/aria-description on a native control (release-plan M5 follow-up,
 *  docs/keyboard-accessibility.md "aria-label on native controls"): every widget wrapper below
 *  forwards `accessibleName`/`accessibleDescription` onto the INNER QtQuick.Templates control that
 *  actually takes focus (button.cpp's T.Button, textinputs.cpp's T.TextField/T.TextArea, …) — this
 *  just emits the two lines on the W.* instance when the author passed them. */
function emitAriaForNativeControl(
  props: Props, scope: Scope, pad: string,
  names: { name: string; description: string } = { name: "accessibleName", description: "accessibleDescription" },
): string[] {
  const lines: string[] = [];
  if (props.ariaLabel) lines.push(`${pad}${names.name}: ${emitExpr(props.ariaLabel, { ...scope, mode: "binding" })}`);
  if (props.ariaDescription !== undefined) lines.push(`${pad}${names.description}: ${JSON.stringify(props.ariaDescription)}`);
  return lines;
}

// ARIA role → QAccessible::Role, exposed to QML as `Accessible.<Role>` (QQuickAccessibleAttached's
// QML_EXTENDED_NAMESPACE(QAccessible)). Only roles with a direct, unambiguous Qt Quick equivalent —
// an unmapped role WARNS (isWarnOnlyAttr's doc comment) instead of silently doing nothing AND
// instead of failing the build (the attribute's other effects — onClick, focus — still apply).
const ARIA_ROLE_MAP: Record<string, string> = {
  button: "Button", link: "Link", checkbox: "CheckBox", switch: "Switch", tab: "PageTab",
  tablist: "PageTabList", menuitem: "MenuItem", listitem: "ListItem", list: "List",
  heading: "Heading", dialog: "Dialog", alert: "AlertMessage", img: "Graphic", textbox: "EditableText",
};

// The app-wide tab-navigation switch (src/shims/tabstop.h's `solidTabstop` context property),
// guarded exactly like every other native widget's activeFocusOnTab (button.cpp) — a generated
// component may run standalone (a transpiler test, the twin-render harness) where it is undefined.
const TABSTOP_GUARD = `typeof solidTabstop !== "undefined" && solidTabstop ? solidTabstop.enabled : true`;

/** An ARIA boolean value: `aria-hidden="true"` (the DOM's string convention), a bare boolean
 *  literal, or a reactive expression — Accessible.<state> (or `enabled` for aria-disabled) takes
 *  a real JS boolean, so a literal "true"/"false" STRING is normalised, not just truthied. */
function ariaBoolExpr(value: t.Node, scope: Scope): string {
  if (t.isBooleanLiteral(value)) return value.value ? "true" : "false";
  if (t.isStringLiteral(value)) return value.value === "true" ? "true" : "false";
  return emitExpr(value as t.Expression, { ...scope, mode: "binding" });
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

/** <Show when={C} fallback={F}>children</Show> → LAZY, like Solid's Show: the children exist only
 *  while C is truthy (created when it turns true, destroyed when it turns false) and the fallback
 *  only while it is falsy. Each child becomes `Repeater { model: (C) ? 1 : 0 }` whose delegate is
 *  the child — the mechanism <Index> uses: the Repeater parents its delegate to the containing box's
 *  content holder, so it is a DIRECT layout child exactly as before, and creation is SYNCHRONOUS (a
 *  handler that opens a Show and then focuses a ref inside it still finds the element). The old
 *  emission only toggled `visible`, so every hidden subtree — dialogs, toolbars, whole panels — was
 *  built at startup anyway. */
function emitShow(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, outerGuard?: string): string[] {
  const when = readWhen(propsArg, scope);
  const guard = outerGuard ? `(${outerGuard}) && (${when})` : when;
  const inverse = outerGuard ? `(${outerGuard}) && !(${when})` : `!(${when})`;
  const out: string[] = [];
  for (const child of children) out.push(...lazyGate(child, guard, scope, level));
  for (const fb of readFallback(propsArg, scope)) out.push(...lazyGate(fb, inverse, scope, level));
  return out;
}

/** Tags that are not Items (a Repeater can only create Items): under a <Show> they keep the guard
 *  as their own `enabled` instead of being mounted lazily. */
const NON_ITEM_TAGS = new Set(["Shortcut"]);

/** One `Repeater { model: (cond) ? 1 : 0 }` per element (a fragment gates each of its children). */
function lazyGate(node: t.Node, cond: string, scope: Scope, level: number): string[] {
  if (!isHCall(node)) {
    if (isWhitespaceString(node) || t.isNullLiteral(node) || t.isBooleanLiteral(node)) return [];
    throw new Error("<Show>: a bare text/expression child (or fallback) is not emitted natively — wrap it in <text>…</text>");
  }
  const { tag } = hParts(node);
  if (isFragmentTag(tag)) return hParts(node).children.flatMap((k) => lazyGate(k as t.Node, cond, scope, level));
  if (t.isIdentifier(tag) && NON_ITEM_TAGS.has(tag.name)) return emitQml(node as t.CallExpression, scope, level, cond);
  const pad = INDENT.repeat(level);
  const body = publishLazyRefs(emitQml(node, scope, level + 1), scope);
  if (!body.length) return [];
  // A Repeater's default property is `delegate` (ONE root): a child that emits several roots — a
  // nested <Show> with several children or a fallback — would keep only its last one. Web semantics
  // are a FRAGMENT (siblings in the parent's flow), so each root gets its own gate on the same
  // condition; no implicit wrapper box (that would change the parent's flex layout).
  return splitRoots(body, level + 1, "<Show> child").flatMap((root) =>
    [`${pad}Repeater {`, `${pad}${INDENT}model: (${cond}) ? 1 : 0`, ...root, `${pad}}`]);
}

/** Split emitted lines into their top-level objects at `level` (each `Type {` … `}` block). A line
 *  at that level that is not an object (a stray property) means the output cannot be placed in a
 *  single-root slot faithfully: fail loud. Non-Item roots (Shortcut) cannot be a Repeater/Component
 *  delegate root either. `what` names the slot in the error. */
export function splitRoots(lines: string[], level: number, what: string): string[][] {
  const pad = INDENT.repeat(level);
  const roots: string[][] = [];
  for (const l of lines) {
    if (l.startsWith(pad) && !l.startsWith(pad + " ")) {
      const own = l.slice(pad.length);
      if (own === "}") { roots[roots.length - 1]?.push(l); continue; }
      const m = /^([\w.]+) \{\s*$/.exec(own);
      if (!m) throw new Error(`${what}: cannot place \`${own.trim()}\` as a delegate root`);
      if (NON_ITEM_TAGS.has(m[1])) throw new Error(`${what}: <${m[1]}> is not an Item and cannot be mounted lazily here; move it out of the nested control flow`);
      roots.push([l]);
      continue;
    }
    if (!roots.length) throw new Error(`${what}: unexpected emitted line \`${l.trim()}\``);
    roots[roots.length - 1].push(l);
  }
  return roots;
}

/** A <For>/<Index>/<VirtualList> row is ONE delegate object per model entry: several roots (a
 *  fragment row, a <Show> with a fallback as the row) have no faithful mapping — fail loud. */
function requireSingleRoot(rows: string[], level: number, owner: string): void {
  const n = splitRoots(rows, level, `<${owner}> row`).length;
  if (n !== 1)
    throw new Error(`<${owner}> row must have a single root element, got ${n} (a fragment or a <Show>/<Switch> with several branches as the row) — wrap the row in a <div>`);
}

/** A ref'd element inside a lazy delegate: its id is local to the delegate, so it publishes itself
 *  into the component-root property `_ref_<x>` (declared by emitComponentType) while it lives. Uses
 *  inside the delegate switch to the local id; uses elsewhere read the root property unchanged. */
function publishLazyRefs(lines: string[], scope: Scope): string[] {
  let out = lines;
  for (let i = 0; i < out.length; i++) {
    const m = /^(\s*)id: (_ref_\w+)\s*$/.exec(out[i]);
    if (!m || m[2].endsWith("__lazy")) continue;
    const [, ws, name] = m;
    if (!scope.lazyRefs) throw new Error(`ref ${name} inside <Show>: no component root to publish it on`);
    // The element must not already carry its own completion handler (one per object in QML).
    for (let j = i + 1; j < out.length && out[j].startsWith(ws) && !/^\s*\}/.test(out[j].slice(0, ws.length + 1)); j++) {
      if (out[j].startsWith(`${ws}Component.onCompleted:`) || out[j].startsWith(`${ws}Component.onDestruction:`))
        throw new Error(`ref ${name} inside <Show>: its element already has a Component completion handler`);
    }
    const local = `${name}__lazy`;
    const word = new RegExp(`\\b${name}\\b`, "g");
    out = out.map((l) => l.replace(word, local));
    out.splice(i + 1, 0,
      `${ws}Component.onCompleted: ${name} = ${local}`,
      `${ws}Component.onDestruction: if (${name} === ${local}) ${name} = null`);
    scope.lazyRefs.add(name);
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
 *  Row identity (C23) is Solid web's: the element's REFERENCE — a surviving element keeps (moves) its
 *  row, a new object is a new row even with the same id (the old row's state and onCleanup go with it).
 *  `key="id"` (or `key={(r) => r.id}`) opts into reuse by that field: a new object with a known key
 *  updates the existing row in place. Emitted as the repeater's `key` (no line without one). */
function emitFor(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const each = readEach(propsArg, scope);
  const key = forKeyField(propsArg);
  const pad = INDENT.repeat(level);
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  // 5v-1 (virtualized CssRepeater): a certified row gets `virtualize: true` right after `model:`,
  // automatically (owner rule: only transpiler-proven passive rows). The engine still windows the
  // list only when it is runtime-eligible (fixed-pitch rows in a vertical scroll column); otherwise it
  // stays eager. Uncertified rows emit exactly as before.
  const virtualize = !!delegate && certifyForRow(delegate);
  const lines = [
    `${pad}Css.CssRepeater {`,
    ...guardLine(guard, level),
    `${pad}${INDENT}model: ${each}`,
    ...(key ? [`${pad}${INDENT}key: ${JSON.stringify(key)}`] : []),
    ...(virtualize ? [`${pad}${INDENT}virtualize: true`] : []),
  ];
  if (delegate) {
    lines.push(`${pad}${INDENT}delegate: Component {`);
    lines.push(...rowDelegate(delegate, scope, level + 2, "For"));
    lines.push(`${pad}${INDENT}}`);
  }
  lines.push(`${pad}}`);
  return lines;
}

/** A <For>/<VirtualList> row: the delegate's element with its root at `rootLevel`, the row item
 *  bound to `modelData`. Every delegate binds its row to `modelData`, so a loop nested in this one
 *  SHADOWS it. When the row is used inside a nested <For>/<Index>/lazy <Show>, the delegate root
 *  publishes it as a property under an id, and every use goes through that id — QML resolves ids
 *  up the context chain, so the inner delegate still sees it. Without a nested use the emission
 *  is unchanged. `extra`: lines for the root (after the publish lines), at rootLevel + 1. */
function rowDelegate(delegate: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope, rootLevel: number,
                     owner: string, extra: string[] = []): string[] {
  const [param, indexParam] = delegate.params;
  const itemName = param && t.isIdentifier(param) ? param.name : null;
  // <For>'s SECOND delegate param is an INDEX ACCESSOR (Solid: `i()` reads the row's current
  // index — unlike <Index>, where the second param is a plain number). Routed through
  // scope.accessors, same mechanism <Index> uses for its row-value accessor: a CALL `i()`
  // resolves to the fixed expression (expr.ts), and a BARE (uncalled) reference — `i` passed as a
  // value — resolves to a closure returning it (expr.ts), matching "accessor function" semantics.
  const indexName = indexParam && t.isIdentifier(indexParam) ? indexParam.name : null;
  const body = delegate.body;
  if (!isHCall(body)) throw new Error(`${owner} delegate must return a single element in this plan`);
  let rowRef = "modelData";
  // The row's index is ALWAYS routed through this reserved per-row property — never through the
  // bare QML `index` context property directly — EVEN when nothing in this row's body nests into
  // another loop. Reason: a NATIVE handler the emitter itself generates elsewhere in this same
  // row (e.g. <select onChange> → `onActivated: (index) => …`, or a ListView/TableView
  // `onSelected: (item, index) => …`) declares its own JS parameter literally named `index` — an
  // ordinary ECMAScript closure, which SHADOWS the QML `index` context property for every
  // identifier inside its body, including a nested `i()` call, at the plain-JS-scoping level (no
  // QML magic can see through that). `property int __forIndex: index` snapshots the delegate's
  // own `index` context property under a name no synthetic handler will ever declare as its own
  // parameter, so no handler/inner-scope `index` can shadow it. (Real case: solid-mail's
  // MailApp.tsx — `<For each={fConds()}>{(c, i) => <select onChange={(e) => condSet(i(), …)}>`.)
  let indexRef = indexName ? "__forIndex" : "index";
  let publish: string | null = null;
  if ((itemName && usedInNestedLoop(body, itemName)) || (indexName && usedInNestedLoop(body, indexName))) {
    const counter = scope.hoverCounter ?? { n: 0 };
    publish = `__for${counter.n++}`;
    rowRef = `${publish}.__forItem`;
    indexRef = `${publish}.__forIndex`;
  }
  const inner: Scope = {
    ...scope,
    locals: { ...(scope.locals ?? {}), ...(itemName ? { [itemName]: rowRef } : {}) },
    accessors: { ...(scope.accessors ?? {}), ...(indexName ? { [indexName]: indexRef } : {}) },
  };
  const rows = emitQml(body, inner, rootLevel);
  requireSingleRoot(rows, rootLevel, owner);
  // The delegate is ONE h() call, so rows[0] is its root's `Type {` line; the publish lines go
  // right under it. Fail loud if that ever stops being true.
  if ((publish || indexName || extra.length) && !rows[0]?.trimEnd().endsWith("{"))
    throw new Error(`${owner} delegate root: expected \`Type {\` as the first emitted line`);
  const rootPad = INDENT.repeat(rootLevel + 1);
  const head: string[] = [];
  if (publish) {
    // Reuse the root's own id if it has one (a ref / drag source): an object takes only one.
    const own = rows.find((l) => l.startsWith(`${rootPad}id: `));
    if (own) {
      const id = own.slice(`${rootPad}id: `.length).trim();
      // `${publish}.__for` is the common prefix of BOTH `__forItem` and `__forIndex` — one
      // replace covers whichever (or both) of the published properties the body references.
      const fixed = rows.map((l) => l.split(`${publish}.__for`).join(`${id}.__for`));
      rows.splice(0, rows.length, ...fixed);
    } else {
      head.push(`${rootPad}id: ${publish}`);
    }
    head.push(`${rootPad}property var __forItem: modelData`);
  }
  // Declared whenever the delegate HAS an index param at all — not only when crossing into a
  // nested loop — so i() never has to fall back to the bare (shadowable) `index` (see the comment
  // on `indexRef` above).
  if (indexName) head.push(`${rootPad}property int __forIndex: index`);
  rows.splice(1, 0, ...head, ...extra.map((l) => rootPad + l));
  return rows;
}

/** <VirtualList each={E} class="…">{(item) => <row/>}</VirtualList> → a VIRTUALIZED list: a QtQuick
 *  ListView in a CSS box (the box takes its size from the layout, like any div). Only the rows in
 *  view (plus a cache margin) exist, and they are REUSED — scrolling, or a new `each` (another
 *  mailbox, a refresh), moves rows' modelData instead of creating/destroying them. For long lists
 *  whose <For> would build every row (each one a whole subtree of CSS boxes, texts, areas).
 *  Rows lay out with their own CSS, as wide as the list, as tall as their content. */
function emitVirtualList(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const each = readEach(propsArg, scope);
  const props = readProps(propsArg);
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (!delegate) throw new Error("<VirtualList> takes a row function: {(item) => <row/>}");
  const refLine = props.ref ? [`${i(1)}id: _ref_${safeName(props.ref)}`] : [];
  const lvId = `__vlist${(scope.hoverCounter ?? { n: 0 }).n++}`;
  if (props.ref && scope.refs) scope.refs.push(props.ref);
  // onEndReached={() => …}: the scroll reached the last row — a long list asks for its next page.
  let endHandler: string | null = null;
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "onEndReached" })) endHandler = emitHandler(p.value, scope);
  }
  const keyFn = props.onKeyDown;
  if (keyFn && !(t.isArrowFunctionExpression(keyFn) || t.isFunctionExpression(keyFn)))
    throw new Error("onKeyDown must be an inline function: (e) => …");
  return [
    `${pad}W.Div {`,
    ...refLine,
    ...buildCssClassLine(props, scope, i(1)),
    ...guardLine(guard, level),
    // ref.reveal(i): scroll row i into view (keyboard moves the selection off-screen).
    `${i(1)}function reveal(i) { if (i >= 0) ${lvId}.positionViewAtIndex(i, ListView.Contain) }`,
    ...(keyFn ? [`${i(1)}Keys.onPressed: (event) => { ${translateKeyHandler(keyFn as t.ArrowFunctionExpression, scope)} }`] : []),
    `${i(1)}ListView {`,
    `${i(2)}id: ${lvId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}clip: true`,
    `${i(2)}reuseItems: true`,
    `${i(2)}cacheBuffer: 600`,
    `${i(2)}boundsBehavior: Flickable.StopAtBounds`,
    // Desktop wheel: discrete steps, no fling — the engine's own scroll (cssscroll.h), a longer
    // step: a list row is tall. Flickable's velocity-based wheel crawled. Trackpad pixels ×1.5.
    `${i(2)}WheelHandler {`,
    `${i(3)}acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad`,
    // (Declared inside a ListView, the handler's parent is the contentItem: the view is named.)
    `${i(3)}onWheel: (ev) => { var lv = ${lvId}; var dy = ev.pixelDelta.y !== 0 ? ev.pixelDelta.y * 1.5 : (ev.angleDelta.y / 120) * 110; lv.contentY = Math.max(lv.originY, Math.min(lv.contentY - dy, lv.originY + lv.contentHeight - lv.height)) }`,
    `${i(2)}}`,
    `${i(2)}model: ${each}`,
    ...(endHandler ? [`${i(2)}onAtYEndChanged: if (atYEnd && count > 0) { ${endHandler} }`] : []),
    `${i(2)}delegate: Component {`,
    ...rowDelegate(delegate, scope, level + 3, "VirtualList", ["width: ListView.view ? ListView.view.width : 0"]),
    `${i(2)}}`,
    `${i(1)}}`,
    `${pad}}`,
  ];
}

/** Does a subtree hold an element that takes clicks itself — its own onClick, or a native
 *  control (<button>, <input>, <textarea>, <select>)? A shallow AST walk over the h() calls. */
// Unknown component/control-flow bodies may contain hover areas too. Known passive
// element trees keep the existing MouseArea; no extra observer for ordinary text content.
function hasHoverAreaDescendant(nodes: t.Node[]): boolean {
  const areas = new Set(["button", "input", "textarea", "select"]);
  const pointerProps = new Set(["onClick", "onContextMenu", "draggable", "title", "onMouseEnter", "onMouseLeave"]);
  const walk = (n: t.Node): boolean => {
    if (isHCall(n)) {
      const { tag, props } = hParts(n);
      if (!t.isStringLiteral(tag) || areas.has(tag.value)) return true;
      if (t.isObjectExpression(props) && props.properties.some(p =>
        t.isSpreadElement(p) || (t.isObjectProperty(p) && t.isIdentifier(p.key) && pointerProps.has(p.key.name)))) return true;
    }
    for (const key of t.VISITOR_KEYS[n.type] ?? []) {
      const value = (n as unknown as Record<string, t.Node | t.Node[]>)[key];
      if (Array.isArray(value) ? value.some(v => v && walk(v)) : value && walk(value)) return true;
    }
    return false;
  };
  return nodes.some(walk);
}

function hasInteractiveDescendant(nodes: t.Node[]): boolean {
  const NATIVE = new Set(["button", "input", "textarea", "select"]);
  let found = false;
  const walk = (n: unknown) => {
    if (found || !n || typeof n !== "object") return;
    if (Array.isArray(n)) { for (const x of n) walk(x); return; }
    const nn = n as t.Node;
    if (!(nn as { type?: string }).type) return;
    if (isHCall(nn)) {
      const [tag, props] = (nn as t.CallExpression).arguments;
      if (t.isStringLiteral(tag) && NATIVE.has(tag.value)) { found = true; return; }
      if (props && t.isObjectExpression(props)
          && props.properties.some((p) => t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "onClick" }))) {
        found = true;
        return;
      }
    }
    for (const key of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[key]);
  };
  walk(nodes);
  return found;
}

/** Is `name` referenced inside a nested Repeater delegate somewhere in `node` — a <For>/<Index>,
 *  or a lazy <Show>/<Switch> (each gates its children with `Repeater { model: (C) ? 1 : 0 }`,
 *  whose delegate binds its OWN `modelData`, the number 0)? (A shallow AST walk: any identifier
 *  with that name under such an h() call counts — shadowing by an inner parameter of the same
 *  name is rare enough to over-approximate, and only costs an id.) */
function usedInNestedLoop(node: t.Node, name: string): boolean {
  let found = false;
  const walk = (n: unknown, nested: boolean) => {
    if (found || !n || typeof n !== "object") return;
    if (Array.isArray(n)) { for (const x of n) walk(x, nested); return; }
    const nn = n as t.Node;
    if (!(nn as { type?: string }).type) return;
    if (nested && t.isIdentifier(nn) && nn.name === name) { found = true; return; }
    let isLoop = false;
    if (isHCall(nn)) {
      const tag = (nn as t.CallExpression).arguments[0];
      isLoop = t.isIdentifier(tag) && ["For", "VirtualList", "Index", "Show", "Switch"].includes(tag.name);
    }
    for (const key of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[key], nested || isLoop);
  };
  walk(node, false);
  return found;
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
    const body = delegate.body;
    if (!isHCall(body)) throw new Error("Index delegate must return a single element in this plan");
    // As in <For>: a nested Repeater (a loop, a lazy <Show>) shadows `modelData` and `index`, so
    // a row or index used inside one is published on the delegate root and read through its id.
    let publish: string | null = null;
    if ((itemName && usedInNestedLoop(body, itemName)) || (idxName && usedInNestedLoop(body, idxName))) {
      const counter = scope.hoverCounter ?? { n: 0 };
      publish = `__for${counter.n++}`;
    }
    const inner: Scope = {
      ...scope,
      accessors: { ...(scope.accessors ?? {}), ...(itemName ? { [itemName]: publish ? `${publish}.__forItem` : "modelData" } : {}) },
      locals: { ...(scope.locals ?? {}), ...(idxName ? { [idxName]: publish ? `${publish}.__forIndex` : "index" } : {}) },
    };
    const rows = emitQml(body, inner, level + 1);
    requireSingleRoot(rows, level + 1, "Index");
    if (publish) {
      if (!rows[0]?.trimEnd().endsWith("{")) throw new Error("Index delegate root: expected `Type {` as the first emitted line");
      const rootPad = INDENT.repeat(level + 2);
      const own = rows.find((l) => l.startsWith(`${rootPad}id: `));
      const props = [`${rootPad}property var __forItem: modelData`, `${rootPad}property int __forIndex: index`];
      if (own) {
        const id = own.slice(`${rootPad}id: `.length).trim();
        const fixed = rows.map((l) => l.split(`${publish}.__for`).join(`${id}.__for`));
        rows.splice(0, rows.length, ...fixed);
        rows.splice(1, 0, ...props);
      } else {
        rows.splice(1, 0, `${rootPad}id: ${publish}`, ...props);
      }
    }
    lines.push(...rows);
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
function emitSwitch(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, outerGuard?: string): string[] {
  const out: string[] = [];
  const priors: string[] = [];
  const pad = INDENT.repeat(level);
  // One lazy incubator per root of a branch, on the branch's guard.
  const branch = (node: t.Node, guard: string) => {
    for (const root of splitRoots(emitQml(node as t.CallExpression, scope, level + 2), level + 2, "<Match> child")) {
      out.push(`${pad}Css.CssIncubator {`, `${pad}${INDENT}active: (${guard}) ? true : false`);
      out.push(`${pad}${INDENT}sourceComponent: Component {`);
      out.push(...root);
      out.push(`${pad}${INDENT}}`, `${pad}}`);
    }
  };
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
      // A Component holds ONE root: a multi-root body (a nested <Show> with several branches) gets
      // one incubator per root, all on the same guard — siblings, as the web's fragment.
      branch(k, guard);
    }
    priors.push(`(${when})`);
  }
  // <Switch fallback={F}>: mounted while NO Match holds (Solid's Switch fallback), lazily too.
  const fallback = readFallback(propsArg, scope);
  if (fallback.length) {
    const none = priors.length ? `!(${priors.join(" || ")})` : "true";
    const guard = outerGuard ? `(${outerGuard}) && (${none})` : none;
    for (const fb of fallback) {
      if (!isHCall(fb)) throw new Error("<Switch fallback>: expected an element — wrap text in <text>…</text>");
      const { tag, children: fbKids } = hParts(fb);
      if (isFragmentTag(tag)) { for (const k of fbKids) if (isHCall(k)) branch(k, guard); }
      else branch(fb, guard);
    }
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
