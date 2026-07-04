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

/** <ToolBar class="…">children</ToolBar>
 *  → wrapper Css.CssFill (cssPrimitive "toolbar") + T.ToolBar { anchors.fill; background: null }.
 *
 *  Children are emitted into the WRAPPER's default slot (its contentHolder), NOT into the
 *  control's contentItem: the layout engine only flows a Css container's direct contentHolder
 *  children (csslayout.cpp layout() iterates content->childItems()), so hosting them inside the
 *  control's contentItem Item would orphan them from the author's flex/grid rules. The T.ToolBar
 *  is a plain (non-Css) sibling in the same holder — anchored full, skipped by the flex pass
 *  (same coexistence as emitInput's T.TextField). Its contentItem is an empty Item so the
 *  template never instantiates style-less chrome of its own. */
const emitToolBar: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);
  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "toolbar"`,
    `${i(1)}T.ToolBar {`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}background: null`,
    `${i(2)}contentItem: Item { }`,
    `${i(1)}}`,
    ...emitChildren(children, scope, level + 1),
    `${pad}}`,
  ];
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// <TabBar current={i()} onChange={(i)=>…}> with <TabButton>label</TabButton> children
// ─────────────────────────────────────────────────────────────────────────────────────────────────

/** → wrapper Css.CssFill (cssPrimitive "tabbar") + T.TabBar.
 *  contentItem MUST be provided (templates create no contentItem — verified project-wide
 *  pitfall): a ListView over tabBar.contentModel, horizontal, like the Basic style.
 *  Controlled index: a Binding element (restoreMode RestoreNone — emitInput's idiom) re-asserts
 *  the `current` value; onCurrentIndexChanged fires the author's onChange with currentIndex.
 *  Each <TabButton> → T.TabButton with Css slots:
 *    background: CssFill ["tab"], cssState checked→"selected" / hovered→"hover"
 *    contentItem: CssText ["tab-label"] (same cssState — background and contentItem are SIBLING
 *    slots, so ancestor-state scoping cannot reach the label through the background). */
const emitTabBar: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const barId = `__tabbar${n}`;

  const currentExpr = bindExpr(props.get("current"), scope);
  const onChangeFn = asFn(props.get("onChange"));
  const changeBody = onChangeFn ? handlerBody(onChangeFn, scope, `${barId}.currentIndex`) : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "tabbar"`,
    `${i(1)}implicitWidth: ${barId}.implicitWidth`,
    `${i(1)}implicitHeight: ${barId}.implicitHeight`,
    `${i(1)}T.TabBar {`,
    `${i(2)}id: ${barId}`,
    `${i(2)}anchors.fill: parent`,
    ...implicitLines(i),
    `${i(2)}background: null`,
    // Basic-style contentItem: the ListView hosts the buttons from the contentModel.
    `${i(2)}contentItem: ListView {`,
    `${i(3)}model: ${barId}.contentModel`,
    `${i(3)}currentIndex: ${barId}.currentIndex`,
    `${i(3)}spacing: ${barId}.spacing`,
    `${i(3)}orientation: ListView.Horizontal`,
    `${i(3)}boundsBehavior: Flickable.StopAtBounds`,
    `${i(3)}flickableDirection: Flickable.AutoFlickIfNeeded`,
    `${i(3)}snapMode: ListView.SnapToItem`,
    `${i(3)}highlightMoveDuration: 0`,
    `${i(2)}}`,
  ];
  if (changeBody) lines.push(`${i(2)}onCurrentIndexChanged: { ${changeBody} }`);

  // <TabButton> children → T.TabButton entries in the container's contentModel.
  let k = 0;
  for (const child of children) {
    if (!isHCall(child)) continue; // whitespace / comments
    const { tag, children: kids } = hParts(child);
    if (!t.isIdentifier(tag) || tag.name !== "TabButton")
      throw new Error(`<TabBar> children must be <TabButton> elements`);
    const tabId = `__tab${n}_${k++}`;
    const tabState = `(${tabId}.checked ? ["selected"] : []).concat(${tabId}.hovered ? ["hover"] : [])`;
    lines.push(
      `${i(2)}T.TabButton {`,
      `${i(3)}id: ${tabId}`,
      `${i(3)}implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)`,
      `${i(3)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
      `${i(3)}padding: 8`,
      `${i(3)}activeFocusOnTab: solidTabstop.enabled`,
      `${i(3)}background: Css.CssFill {`,
      `${i(4)}cssPrimitive: "div"`,
      `${i(4)}cssClass: ["tab"]`,
      `${i(4)}cssState: ${tabState}`,
      `${i(3)}}`,
      `${i(3)}contentItem: Css.CssText {`,
      `${i(4)}cssPrimitive: ""`,
      `${i(4)}cssClass: ["tab-label"]`,
      `${i(4)}cssState: ${tabState}`,
      `${i(4)}text: ${labelBinding(kids, scope)}`,
      `${i(3)}}`,
      `${i(2)}}`,
    );
  }

  lines.push(`${i(1)}}`);

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

/** → wrapper Css.CssFill (cssPrimitive "splitview") + T.SplitView { orientation }.
 *  Children emit as DIRECT children of the SplitView — the Container manages their geometry
 *  (pane implicit sizes come from each pane's own Css layout). The handle is a styleable
 *  Css.CssRect ["handle"], 6px thick, with hover/active cssState from the SplitHandle attached. */
const emitSplitView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const splitId = `__split${counter.n++}`;

  let orientation = "Qt.Horizontal";
  const o = props.get("orientation");
  if (o && t.isStringLiteral(o)) {
    if (o.value === "vertical") orientation = "Qt.Vertical";
    else if (o.value !== "horizontal") throw new Error(`<SplitView> orientation must be "horizontal" or "vertical", got "${o.value}"`);
  }

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "splitview"`,
    `${i(1)}T.SplitView {`,
    `${i(2)}id: ${splitId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}orientation: ${orientation}`,
    `${i(2)}handle: Css.CssRect {`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["handle"]`,
    `${i(3)}cssState: (T.SplitHandle.pressed ? ["active"] : []).concat(T.SplitHandle.hovered ? ["hover"] : [])`,
    `${i(3)}implicitWidth: ${splitId}.orientation === Qt.Horizontal ? 6 : ${splitId}.width`,
    `${i(3)}implicitHeight: ${splitId}.orientation === Qt.Horizontal ? ${splitId}.height : 6`,
    `${i(2)}}`,
    ...emitChildren(children, scope, level + 2),
    `${i(1)}}`,
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

/** PHASE-1 CONTRACT: children are pages, only child[current] is visible. T.StackView's API is
 *  imperative (push/pop/replace) and does not fit the declarative controlled-index shape, so this
 *  emits a plain Css.CssRect host (cssPrimitive "stack") with per-child `visible` guards — the
 *  layout engine already treats an invisible child as out of flow, so the visible page gets the
 *  full CSS box. Imperative push/pop via refs is a later phase. */
const emitStackView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));

  const currentExpr = bindExpr(props.get("current"), scope) ?? "0";

  const lines: string[] = [
    `${pad}Css.CssRect {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "stack"`,
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

/** → wrapper Css.CssFill (cssPrimitive "swipeview") + T.SwipeView.
 *  contentItem: Basic-style ListView over the contentModel (templates create no contentItem);
 *  clip keeps the neighbouring pages inside the box while swiping. Children are direct children
 *  of the SwipeView — the Container adopts them as pages and resizes each to the view.
 *  Controlled index via Binding (RestoreNone); onCurrentIndexChanged → onChange(currentIndex). */
const emitSwipeView: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const swipeId = `__swipe${counter.n++}`;

  const currentExpr = bindExpr(props.get("current"), scope);
  const onChangeFn = asFn(props.get("onChange"));
  const changeBody = onChangeFn ? handlerBody(onChangeFn, scope, `${swipeId}.currentIndex`) : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "swipeview"`,
    `${i(1)}implicitWidth: ${swipeId}.implicitWidth`,
    `${i(1)}implicitHeight: ${swipeId}.implicitHeight`,
    `${i(1)}T.SwipeView {`,
    `${i(2)}id: ${swipeId}`,
    `${i(2)}anchors.fill: parent`,
    ...implicitLines(i),
    `${i(2)}background: null`,
    `${i(2)}contentItem: ListView {`,
    `${i(3)}model: ${swipeId}.contentModel`,
    `${i(3)}interactive: ${swipeId}.interactive`,
    `${i(3)}currentIndex: ${swipeId}.currentIndex`,
    `${i(3)}spacing: ${swipeId}.spacing`,
    `${i(3)}orientation: ${swipeId}.orientation`,
    `${i(3)}snapMode: ListView.SnapOneItem`,
    `${i(3)}boundsBehavior: Flickable.StopAtBounds`,
    `${i(3)}highlightRangeMode: ListView.StrictlyEnforceRange`,
    `${i(3)}preferredHighlightBegin: 0`,
    `${i(3)}preferredHighlightEnd: 0`,
    `${i(3)}highlightMoveDuration: 250`,
    `${i(3)}clip: true`,
    `${i(2)}}`,
  ];
  if (changeBody) lines.push(`${i(2)}onCurrentIndexChanged: { ${changeBody} }`);
  lines.push(...emitChildren(children, scope, level + 2));
  lines.push(`${i(1)}}`);

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

/** <PageIndicator count={n} current={i()} /> → wrapper CssFill + T.PageIndicator.
 *  delegate: Css.CssRect ["dot"] 8×8 with cssState "selected" on the current page; contentItem
 *  is the Basic-style Row + Repeater (templates create no contentItem). */
const emitPageIndicator: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propMap(propsArg);
  const classLine = buildCssClassLine(cssProps(props), scope, i(1));
  markWidgets(scope);

  const counter = scope.inputCounter ?? { n: 0 };
  const dotsId = `__dots${counter.n++}`;

  const countExpr = bindExpr(props.get("count"), scope) ?? "0";
  const currentExpr = bindExpr(props.get("current"), scope) ?? "0";

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "pageindicator"`,
    `${i(1)}implicitWidth: ${dotsId}.implicitWidth`,
    `${i(1)}implicitHeight: ${dotsId}.implicitHeight`,
    `${i(1)}T.PageIndicator {`,
    `${i(2)}id: ${dotsId}`,
    `${i(2)}anchors.fill: parent`,
    ...implicitLines(i),
    `${i(2)}background: null`,
    `${i(2)}count: ${countExpr}`,
    `${i(2)}currentIndex: ${currentExpr}`,
    `${i(2)}spacing: 6`,
    `${i(2)}delegate: Css.CssRect {`,
    `${i(3)}required property int index`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["dot"]`,
    `${i(3)}cssState: index === ${dotsId}.currentIndex ? ["selected"] : []`,
    `${i(3)}implicitWidth: 8`,
    `${i(3)}implicitHeight: 8`,
    `${i(2)}}`,
    `${i(2)}contentItem: Row {`,
    `${i(3)}spacing: ${dotsId}.spacing`,
    `${i(3)}Repeater {`,
    `${i(4)}model: ${dotsId}.count`,
    `${i(4)}delegate: ${dotsId}.delegate`,
    `${i(3)}}`,
    `${i(2)}}`,
    `${i(1)}}`,
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
