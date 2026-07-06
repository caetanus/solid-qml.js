// Native container tags (plan: 2026-07-03-native-only-widgets, containers group):
//   <ToolBar>            → Css.CssFill "toolbar" + T.ToolBar (semantic chrome)
//   <TabBar>/<TabButton> → T.TabBar + ListView contentItem + T.TabButton per tab
//   <SplitView>          → T.SplitView with a styleable Css handle
//   <Drawer>             → T.Drawer (edge panel) with cssAncestor re-anchoring
//   <StackView>          → phase-1: a plain Css host showing only child[current]
//   <SwipeView>/<PageIndicator> → T.SwipeView + ListView contentItem; dot delegate
//
// All tags are Capitalized QML-only surface (owner directive: no HTML/ARIA contortions).
// Visual slots are filled with Css-engine items; QtQuick.Templates provides behaviour only.
import * as t from "@babel/types";
import { registerNativeTags, type NativeEmit } from "./index.ts";
import { emitChildren, emitQml, buildCssClassLine, guardLine, INDENT } from "../qml.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { hParts, isHCall } from "../../ast/h.ts";

type Fn = t.ArrowFunctionExpression | t.FunctionExpression;

/** All props as raw expressions, keyed by name. */
function propMap(propsArg: t.Node | undefined): Map<string, t.Expression> {
  const m = new Map<string, t.Expression>();
  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) m.set(p.key.name, p.value);
    }
  }
  return m;
}

/** The class/classList shape buildCssClassLine expects (structural subset of qml.ts's Props). */
function cssProps(props: Map<string, t.Expression>) {
  const classes: string[] = [];
  const classList: Array<{ key: string; expr: t.Expression }> = [];
  const cls = props.get("class");
  if (cls && t.isStringLiteral(cls)) classes.push(...cls.value.split(/\s+/).filter(Boolean));
  const list = props.get("classList");
  if (list && t.isObjectExpression(list)) {
    for (const p of list.properties) {
      if (!t.isObjectProperty(p)) continue;
      const key = t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? p.key.value : null;
      if (key && t.isExpression(p.value)) classList.push({ key, expr: p.value });
    }
  }
  return { classes, classList, onClick: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
}

function asFn(e: t.Expression | undefined): Fn | null {
  return e && (t.isArrowFunctionExpression(e) || t.isFunctionExpression(e)) ? e : null;
}

function bindExpr(e: t.Expression | undefined, scope: Scope): string | null {
  return e ? emitExpr(e, { ...scope, mode: "binding" }) : null;
}

/** Translate an author handler to a QML statement string; the first parameter (when
 *  `paramExpr` is given) is aliased to that QML expression — e.g. onChange={(i) => setTab(i)}
 *  with paramExpr `__tabbar0.currentIndex`. Same shape as qml.ts's translateValueHandler. */
function handlerBody(fn: Fn, scope: Scope, paramExpr?: string): string {
  const paramName = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const inner: Scope = {
    ...scope, mode: "handler",
    locals: { ...(scope.locals ?? {}), ...(paramName && paramExpr ? { [paramName]: paramExpr } : {}) },
  };
  if (t.isBlockStatement(fn.body)) {
    return fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  }
  return emitExpr(fn.body, inner);
}

/** String-valued binding from text/interpolation children (local clone of qml.ts's textBinding). */
function labelBinding(children: t.Node[], scope: Scope): string {
  const parts: string[] = [];
  for (const child of children) {
    if (t.isStringLiteral(child)) {
      if (child.value.trim() || /\s/.test(child.value)) parts.push(JSON.stringify(child.value));
    } else if (child.type === "JSXText") {
      const cleaned = (child as unknown as { value: string }).value.replace(/\s+/g, " ");
      if (cleaned.trim()) parts.push(JSON.stringify(cleaned));
    } else if (t.isExpression(child)) {
      parts.push(`(${emitExpr(child, { ...scope, mode: "binding" })})`);
    }
  }
  if (parts.length === 0) return `""`;
  if (parts.length === 1 && parts[0].startsWith('"')) return parts[0];
  return parts[0].startsWith('"') ? parts.join(" + ") : `"" + ${parts.join(" + ")}`;
}

function markWidgets(scope: Scope): void {
  if (scope.usedWidgets) scope.usedWidgets.flag = true;
}

/** Mark that a solidqml.Widgets component was instantiated → prepend `import solidqml.Widgets`.
 *  Migrated widgets host their T.* control in the .qml, so the emitted output needs the W module,
 *  not the raw Templates import. */
function markWidgetLib(scope: Scope): void {
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
}

// Basic-style implicit size policy — Templates set NO implicit sizes themselves (that is the
// style's job, and we ARE the style); without these a container opens 0x0.
function implicitLines(i: (n: number) => string): string[] {
  return [
    `${i(2)}implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)`,
    `${i(2)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
  ];
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <ToolBar> — semantic native chrome; the CSS engine owns paint AND layout.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** <ToolBar class="…">children</ToolBar> → W.ToolBar.
 *  One .qml per component: the CssFill "toolbar" wrapper + T.ToolBar (semantic chrome, nulled
 *  background, empty contentItem) live in ToolBar.qml. The emit only wires the author classes and
 *  passes the children into the component's default content slot. */
const emitToolBar: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);
  return [
    `${pad}W.ToolBar {`,
    ...classLine,
    ...guardLine(guard, level),
    ...emitChildren(children, scope, level + 1),
    `${pad}}`,
  ];
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <TabBar current={i()} onChange={(i)=>…}> with <TabButton>label</TabButton> children
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** <TabBar current={i()} onChange={(i)=>…}> → W.TabBar.
 *  One .qml per component: the CssFill "tabbar" wrapper, the T.TabBar and its Basic-style ListView
 *  contentItem live in TabBar.qml; each <TabButton> becomes a W.TabButton (its own component, with
 *  the ["tab"]/["tab-label"] Css slots). The emit keeps the id `__tabbarN` so the onChange handler's
 *  value-read (`__tabbarN.currentIndex`) and the controlled RestoreNone Binding resolve against the
 *  component's two-way `currentIndex` alias. onCurrentIndexChanged re-emits via that alias, so the
 *  author's onChange fires; the <TabButton> children route into the control's contentData. */
const emitTabBar: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const barId = `__tabbar${counter.n++}`;

  const currentExpr = bindExpr(props.get("current"), scope);
  const onChangeFn = asFn(props.get("onChange"));
  const changeBody = onChangeFn ? handlerBody(onChangeFn, scope, `${barId}.currentIndex`) : "";

  const lines: string[] = [
    `${pad}W.TabBar {`,
    `${i(1)}id: ${barId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];
  if (changeBody) lines.push(`${i(1)}onCurrentIndexChanged: { ${changeBody} }`);

  // <TabButton> children → W.TabButton entries (their root lands in the control's contentData).
  for (const child of children) {
    if (!isHCall(child)) continue; // whitespace / comments
    const { tag, children: kids } = hParts(child);
    if (!t.isIdentifier(tag) || tag.name !== "TabButton")
      throw new Error(`<TabBar> children must be <TabButton> elements`);
    lines.push(
      `${i(1)}W.TabButton {`,
      `${i(2)}text: ${labelBinding(kids, scope)}`,
      `${i(1)}}`,
    );
  }

  if (currentExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${barId}`,
      `${i(2)}property: "currentIndex"`,
      `${i(2)}value: ${currentExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
};

/** <TabButton> outside a <TabBar> has no host container — fail loudly at transpile time. */
const emitTabButtonError: NativeEmit = () => {
  throw new Error(`<TabButton> is only valid as a direct child of <TabBar>`);
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <SplitView orientation="horizontal|vertical">
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** <SplitView orientation="horizontal|vertical"> → W.SplitView.
 *  One .qml per component: the CssFill "splitview" wrapper, the T.SplitView and its SplitHandle
 *  (module-local) live in SplitView.qml. The emit passes the orientation and the panes (into the
 *  control's contentData via the default `panes` alias). It still INJECTS the per-pane
 *  `T.SplitView.fillWidth/fillHeight` hint on each pane's own object: QQuickSplitView reserves space
 *  only for panes that carry a size hint (else the first pane's content-implicit eats the row). That
 *  attached-property reference keeps the Templates import in the generated file. */
const emitSplitView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);
  markWidgets(scope); // the per-pane T.SplitView attached hint needs the Templates import

  let orientation = "Qt.Horizontal";
  const o = props.get("orientation");
  if (o && t.isStringLiteral(o)) {
    if (o.value === "vertical") orientation = "Qt.Vertical";
    else if (o.value !== "horizontal") throw new Error(`<SplitView> orientation must be "horizontal" or "vertical", got "${o.value}"`);
  }

  // Inject `T.SplitView.fillWidth/fillHeight: true` into every pane (after its opening brace) so they
  // share the space and are resizable by the handle. Emit children one at a time so the hint lands on
  // each pane's own object.
  const fillProp = orientation === "Qt.Horizontal" ? "fillWidth" : "fillHeight";
  const paneLines: string[] = [];
  for (const child of children) {
    const emitted = emitChildren([child], scope, level + 1);
    const braceIdx = emitted.findIndex((l) => /\{\s*$/.test(l));
    if (braceIdx >= 0) emitted.splice(braceIdx + 1, 0, `${i(2)}T.SplitView.${fillProp}: true`);
    paneLines.push(...emitted);
  }

  return [
    `${pad}W.SplitView {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}orientation: ${orientation}`,
    ...paneLines,
    `${pad}}`,
  ];
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <Drawer open={sig()} edge="left|right|top|bottom" size={0.34} onClose={…}>
// ─────────────────────────────────────────────────────────────────────────────────────────────────

const EDGE = { left: "Qt.LeftEdge", right: "Qt.RightEdge", top: "Qt.TopEdge", bottom: "Qt.BottomEdge" } as const;

/** → an in-tree Css.CssItem anchor (carries the author's classes for selector scoping) hosting
 *  T.Drawer. Drawer contents reparent to the window Overlay, severing the visual chain author
 *  CSS matches against — background AND contentItem each carry `property Item cssAncestor`
 *  pointing back at the anchor (MANDATORY popup pitfall; they are sibling slots, so the two
 *  properties cover every descendant). No popupType Window — a drawer is an in-window panel.
 *
 *  Controlled visibility: Binding on `visible` (restoreMode RestoreNone); Qt-side closes
 *  (Esc / press outside) fire onClosed → the author's onClose keeps the signal in sync.
 *  The cross axis is NOT sized automatically by QQuickDrawer (its doc examples set both), so
 *  left/right: width = overlay.width × size (default 0.34), height = overlay.height; top/bottom
 *  transposed. contentItem is a Css container so the author's children flow through normal
 *  CSS layout inside the panel. */
const emitDrawer: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const ctlId = `__drawer${n}`;
  const wrapId = `${ctlId}W`;

  let edge: keyof typeof EDGE = "left";
  const e = props.get("edge");
  if (e && t.isStringLiteral(e)) {
    if (!(e.value in EDGE)) throw new Error(`<Drawer> edge must be left|right|top|bottom, got "${e.value}"`);
    edge = e.value as keyof typeof EDGE;
  }
  const horizontal = edge === "left" || edge === "right";
  const size = bindExpr(props.get("size"), scope) ?? "0.34";

  const openExpr = bindExpr(props.get("open"), scope) ?? "false";
  const visibleValue = guard ? `!!(${guard}) && !!(${openExpr})` : `!!(${openExpr})`;
  const onCloseFn = asFn(props.get("onClose"));
  const closeBody = onCloseFn ? handlerBody(onCloseFn, scope) : "";

  const lines: string[] = [
    // Zero-size in-tree anchor: carries the author's classes so `.my-drawer .panel` rules
    // scope through the cssAncestor re-anchor below; paints nothing, flows as an empty box.
    `${pad}Css.CssItem {`,
    ...classLine,
    `${i(1)}id: ${wrapId}`,
    `${i(1)}cssPrimitive: "drawer"`,
    `${i(1)}T.Drawer {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}parent: T.Overlay.overlay`,
    `${i(2)}edge: ${EDGE[edge]}`,
    // Desktop semantics: no edge-swipe open (an interactive drag would fight the controlled
    // Binding on `visible`).
    `${i(2)}dragMargin: 0`,
    `${i(2)}width: parent ? ${horizontal ? `parent.width * (${size})` : "parent.width"} : 0`,
    `${i(2)}height: parent ? ${horizontal ? "parent.height" : `parent.height * (${size})`} : 0`,
    // T.Drawer carries no built-in open/close animation — the STYLE must supply enter/exit
    // transitions that drive `position` 0↔1. Without them `open()`/`visible:true` set
    // `opened:true` but `position` stays 0, leaving the panel anchored off-screen.
    `${i(2)}enter: Transition { NumberAnimation { property: "position"; to: 1.0; duration: 220; easing.type: Easing.OutCubic } }`,
    `${i(2)}exit: Transition { NumberAnimation { property: "position"; to: 0.0; duration: 180; easing.type: Easing.InCubic } }`,
    // Semi-transparent modal scrim (default style provides none → an opaque dim).
    `${i(2)}T.Overlay.modal: Rectangle { color: "#66000000" }`,
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["panel"]`,
    `${i(2)}}`,
    `${i(2)}contentItem: Css.CssFill {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["content"]`,
    ...emitChildren(children, scope, level + 3),
    `${i(2)}}`,
  ];
  if (closeBody) lines.push(`${i(2)}onClosed: { ${closeBody} }`);
  lines.push(
    `${i(1)}}`,
    `${i(1)}Binding {`,
    `${i(2)}target: ${ctlId}`,
    `${i(2)}property: "visible"`,
    `${i(2)}value: ${visibleValue}`,
    `${i(2)}restoreMode: Binding.RestoreNone`,
    `${i(1)}}`,
    `${pad}}`,
  );
  return lines;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <StackView current={i()}> — phase-1 simplified contract
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** PHASE-1 CONTRACT: children are pages, only child[current] is visible → W.StackView.
 *  One .qml per component: the Css.CssRect "stack" host lives in StackView.qml. T.StackView's API is
 *  imperative (push/pop/replace) and does not fit the declarative controlled-index shape, so the host
 *  is a pure Css box; the emit bakes a per-child `visible` guard (`(current) === k`) onto each page —
 *  the layout engine already treats an invisible child as out of flow, so the visible page gets the
 *  full CSS box. Imperative push/pop via refs is a later phase. */
const emitStackView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);

  const currentExpr = bindExpr(props.get("current"), scope) ?? "0";

  const lines: string[] = [
    `${pad}W.StackView {`,
    ...classLine,
    ...guardLine(guard, level),
  ];
  let k = 0;
  for (const child of children) {
    if (!isHCall(child)) continue;
    lines.push(...emitQml(child, scope, level + 1, `(${currentExpr}) === ${k++}`));
  }
  lines.push(`${pad}}`);
  return lines;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <SwipeView current={i()} onChange={(i)=>…}> + <PageIndicator count current>
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** <SwipeView current={i()} onChange={(i)=>…}> → W.SwipeView.
 *  One .qml per component: the CssFill "swipeview" wrapper, the T.SwipeView and its Basic-style
 *  ListView contentItem live in SwipeView.qml. The emit keeps the id `__swipeN` so the onChange
 *  handler's value-read and the controlled RestoreNone Binding resolve against the component's
 *  two-way `currentIndex` alias; the author's pages route into the control's contentData via the
 *  default `pages` alias. */
const emitSwipeView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const swipeId = `__swipe${counter.n++}`;

  const currentExpr = bindExpr(props.get("current"), scope);
  const onChangeFn = asFn(props.get("onChange"));
  const changeBody = onChangeFn ? handlerBody(onChangeFn, scope, `${swipeId}.currentIndex`) : "";

  const lines: string[] = [
    `${pad}W.SwipeView {`,
    `${i(1)}id: ${swipeId}`,
    ...classLine,
    ...guardLine(guard, level),
  ];
  if (changeBody) lines.push(`${i(1)}onCurrentIndexChanged: { ${changeBody} }`);
  lines.push(...emitChildren(children, scope, level + 1));

  if (currentExpr !== null) {
    lines.push(
      `${i(1)}Binding {`,
      `${i(2)}target: ${swipeId}`,
      `${i(2)}property: "currentIndex"`,
      `${i(2)}value: ${currentExpr}`,
      `${i(2)}restoreMode: Binding.RestoreNone`,
      `${i(1)}}`,
    );
  }

  lines.push(`${pad}}`);
  return lines;
};

/** <PageIndicator count={n} current={i()} /> → W.PageIndicator.
 *  One .qml per component: the CssFill "pageindicator" wrapper, the T.PageIndicator, the ["dot"]
 *  delegate and the Basic-style Row+Repeater contentItem live in PageIndicator.qml; `count`/
 *  `currentIndex` are aliases to the control. The emit only wires those two props. */
const emitPageIndicator: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgetLib(scope);

  const countExpr = bindExpr(props.get("count"), scope) ?? "0";
  const currentExpr = bindExpr(props.get("current"), scope) ?? "0";

  return [
    `${pad}W.PageIndicator {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}count: ${countExpr}`,
    `${i(1)}currentIndex: ${currentExpr}`,
    `${pad}}`,
  ];
};

registerNativeTags({
  ToolBar: emitToolBar,
  TabBar: emitTabBar,
  TabButton: emitTabButtonError,
  SplitView: emitSplitView,
  Drawer: emitDrawer,
  StackView: emitStackView,
  SwipeView: emitSwipeView,
  PageIndicator: emitPageIndicator,
});
