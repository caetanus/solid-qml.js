// Native HTML-widget group (plan: 2026-07-03-native-only-widgets, phase 2):
//   <progress>            → T.ProgressBar (determinate + indeterminate)
//   <fieldset>/<legend>   → CssFill box with the legend hoisted first as a CssText
//   <dialog>              → T.Dialog (in-window modal on the Overlay)
//   <details>/<summary>   → disclosure expander (internal __open bool + marker glyph)
//
// Every widget follows the Phase-2..6 canon from ../qml.ts (emitInput / emitCheckboxToggle /
// emitSelect / emitSlider / emitDateInput): a wrapper Css.CssFill carries the CSS identity
// (cssPrimitive + author cssClass + cssState), the T.* template supplies behaviour only
// (background/contentItem chrome nulled or slot-filled with Css items), ids come from the
// shared scope.inputCounter, and `scope.usedWidgets.flag` gates the Templates import.
import * as t from "@babel/types";
import { registerNativeTags } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { emitChildren, buildCssClassLine, guardLine, INDENT } from "../qml.ts";
import { isHCall, hParts } from "../../ast/h.ts";

// ─────────────────────────────────────────────────────────────────────────────────────────
// Local prop/child helpers (readProps/textBinding in qml.ts are module-private — the group
// modules re-implement the minimal subset they need instead of widening qml.ts's surface).
// ─────────────────────────────────────────────────────────────────────────────────────────

/** Props-like subset consumed by buildCssClassLine (structurally compatible with qml.ts Props). */
function readBaseProps(propsArg: t.Node | undefined) {
  const props = {
    classes: [] as string[],
    classList: [] as Array<{ key: string; expr: t.Expression }>,
    onClick: undefined as t.Node | undefined,
    ref: undefined as string | undefined,
    draggable: false,
    dragData: undefined as t.Expression | undefined,
    onDrop: undefined as t.Node | undefined,
  };
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
  }
  return props;
}

/** The raw expression bound to a named prop, or null when absent. */
function findProp(propsArg: t.Node | undefined, name: string): t.Expression | null {
  if (!propsArg || !t.isObjectExpression(propsArg)) return null;
  for (const p of propsArg.properties) {
    if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name }) && t.isExpression(p.value)) return p.value;
  }
  return null;
}

/** A named prop that must be an inline function (arrow/function expression), or null. */
function findFnProp(propsArg: t.Node | undefined, name: string): t.ArrowFunctionExpression | t.FunctionExpression | null {
  const v = findProp(propsArg, name);
  return v && (t.isArrowFunctionExpression(v) || t.isFunctionExpression(v)) ? v : null;
}

/** Emit a handler function's body in handler mode (expression or block body — same statement
 *  flattening as translateInputHandler in qml.ts, without any DOM-event mapping: the handlers
 *  this group wires (onClose) receive no useful event payload). */
function handlerBody(fn: t.ArrowFunctionExpression | t.FunctionExpression, scope: Scope): string {
  const inner: Scope = { ...scope, mode: "handler" };
  if (t.isBlockStatement(fn.body)) {
    return fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  }
  return emitExpr(fn.body, inner);
}

function isJsxText(n: t.Node): boolean {
  return n.type === "JSXText";
}

/** Build a string-valued binding from text + interpolation children (mirror of qml.ts textBinding). */
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

/** Lowercase string tag of an h() call child, or null (identifiers/fragments/non-elements). */
function hTag(node: t.Node): string | null {
  if (!isHCall(node)) return null;
  const { tag } = hParts(node);
  return t.isStringLiteral(tag) ? tag.value : null;
}

// ─────────────────────────────────────────────────────────────────────────────────────────
// <progress value={} max={}> → wrapper CssFill + T.ProgressBar
// ─────────────────────────────────────────────────────────────────────────────────────────

/** <progress value={expr} max={n}> → wrapper Css.CssFill (cssPrimitive "progress") + T.ProgressBar.
 *
 *  HTML semantics: `max` defaults to 1; a <progress> with NO value attribute is indeterminate.
 *  contentItem/background are nulled — the visuals are a track Css.CssRect (cssClass ["track"])
 *  filling the control and a bar Css.CssRect (cssClass ["bar"]) whose width follows
 *  visualPosition (0→1), the same track/fill pattern as emitSlider.
 *
 *  The track lives INSIDE the T.ProgressBar (a plain, non-Css parent) — as a direct wrapper
 *  child it would join the wrapper's CSS flex pass and get repositioned once the author gives
 *  the wrapper box rules. Inside the control, anchors hold unconditionally.
 *
 *  Indeterminate: the bar becomes a 30%-wide segment sliding across the track in an infinite
 *  NumberAnimation loop (from/to are sampled at animation start — good enough for a state that
 *  does not flip mid-run). A "indeterminate" cssState entry lets authors restyle the busy bar.
 *
 *  value is a plain binding (not a Binding element): T.ProgressBar is read-only — no user
 *  interaction ever writes `value` back, so the binding can never be broken. */
function emitProgress(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;
  if (scope.usedWidgets) scope.usedWidgets.flag = true;

  const bind: Scope = { ...scope, mode: "binding" };
  const valueNode = findProp(propsArg, "value");
  const valueExpr = valueNode ? emitExpr(valueNode, bind) : null;
  const maxNode = findProp(propsArg, "max");
  const maxExpr = maxNode ? emitExpr(maxNode, bind) : "1";

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "progress"`,
    `${i(1)}cssState: ${ctlId}.indeterminate ? ["indeterminate"] : []`,
    // Fixed implicit size: with chrome nulled the control reports 0×0 (Templates have no
    // implicit-size policy — that's the style's job, and we ARE the style). CSS overrides.
    `${i(1)}implicitWidth: 200`,
    `${i(1)}implicitHeight: 8`,
    `${i(1)}T.ProgressBar {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    // No visual chrome from Templates; the track/bar CssRects own every painted pixel.
    `${i(2)}contentItem: null`,
    `${i(2)}background: null`,
    `${i(2)}from: 0`,
    `${i(2)}to: ${maxExpr}`,
    ...(valueExpr !== null ? [`${i(2)}value: ${valueExpr}`] : [`${i(2)}indeterminate: true`]),
    // Track: fills the control (== the wrapper); author styles via `.track` (background, radius).
    `${i(2)}Css.CssRect {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["track"]`,
    `${i(3)}anchors.fill: parent`,
    // Bar: the covered portion. A PLAIN Rectangle inside an anchored Item host — a Css child's
    // width binding is CLOBBERED by the layout engine (block child stretches to 100%, the
    // "always full" bug); a plain item is not a layout child, so the visualPosition binding
    // holds. The nested CssItem paints .bar (background-color/radius). Indeterminate: a 30%
    // segment whose x slides across the track in a loop.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Rectangle {`,
    `${i(5)}width: ${ctlId}.indeterminate ? parent.width * 0.3 : ${ctlId}.visualPosition * parent.width`,
    `${i(5)}height: parent.height`,
    `${i(5)}color: "#176b87"`,
    `${i(5)}Css.CssItem { cssPrimitive: "rect"; cssClass: ["bar"] }`,
    `${i(5)}NumberAnimation on x {`,
    `${i(6)}running: ${ctlId}.indeterminate`,
    `${i(6)}from: 0`,
    `${i(6)}to: ${ctlId}.width * 0.7`,
    `${i(6)}duration: 1200`,
    `${i(6)}loops: Animation.Infinite`,
    `${i(5)}}`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
    `${i(1)}}`,
    `${pad}}`,
  ];
}

// ─────────────────────────────────────────────────────────────────────────────────────────
// <fieldset> + <legend>
// ─────────────────────────────────────────────────────────────────────────────────────────

/** <legend>Label</legend> → Css.CssText (cssPrimitive "legend", cssClass ["legend", …author]).
 *  Registered standalone so emitChildren dispatches it wherever it appears; <fieldset> only
 *  reorders it to the front. Text content only (same static-content rule as <option>). */
function emitLegend(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  if (children.some((c) => isHCall(c)))
    throw new Error("<legend> supports text content only — no element children");
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine({ ...props, classes: ["legend", ...props.classes] }, scope, i(1));
  return [
    `${pad}Css.CssText {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "legend"`,
    `${i(1)}text: ${textBinding(children, scope)}`,
    `${pad}}`,
  ];
}

/** <fieldset><legend>…</legend>children</fieldset> → wrapper Css.CssFill (cssPrimitive "fieldset").
 *  No Templates control (T.GroupBox is chrome we would null anyway) — the wrapper renders the
 *  children normally, except the FIRST <legend> child is spliced to the front so it paints first
 *  (as the registered legend CssText). The border/box look is entirely author CSS. */
function emitFieldset(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));

  // Splice the first <legend> child to the front; everything else keeps document order.
  const legendIdx = children.findIndex((c) => hTag(c) === "legend");
  const ordered = legendIdx >= 0
    ? [children[legendIdx], ...children.filter((_, k) => k !== legendIdx)]
    : children;

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "fieldset"`,
    ...emitChildren(ordered, scope, level + 1),
    `${pad}}`,
  ];
}

// ─────────────────────────────────────────────────────────────────────────────────────────
// <dialog open={} onClose={}> → T.Dialog (in-window modal)
// ─────────────────────────────────────────────────────────────────────────────────────────

/** <dialog open={expr} onClose={fn}>children</dialog> → zero-size wrapper CssFill + T.Dialog.
 *
 *  The wrapper only anchors CSS identity/scoping (id target for cssAncestor); the dialog itself
 *  renders on the window Overlay, so the wrapper is 0×0 and never disturbs the parent's layout.
 *
 *  In-window modal (owner directive: NOT popupType Window — a dialog dims and blocks its own
 *  window). anchors.centerIn does not exist on popups (they are not Items): re-parent to
 *  T.Overlay.overlay and center with x/y arithmetic against it.
 *
 *  visible: the literal `visible: <open>` binding is BROKEN the first time the dialog closes
 *  itself (Escape / press-outside call close(), an imperative write) — the sibling Binding
 *  element (restoreMode: RestoreNone) keeps re-asserting the author's open signal afterwards,
 *  the same controlled-value idiom as every input widget.
 *
 *  Popup pitfall (mandatory): dialog contents reparent to the window Overlay, severing the
 *  visual chain author CSS matches against (`.my-dialog .popup`) — `cssAncestor` on BOTH
 *  sibling slots (background and contentItem) re-anchors the engine's ancestor walk at the
 *  wrapper. Templates popups have NO implicit-size policy: implicitWidth/Height from
 *  contentWidth/Height + paddings, or the dialog opens 0×0. */
function emitDialog(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const wrapId = `__dialog${n}W`;
  const dlgId = `__dialog${n}`;
  if (scope.usedWidgets) scope.usedWidgets.flag = true;

  const openNode = findProp(propsArg, "open");
  const openExpr = openNode ? emitExpr(openNode, { ...scope, mode: "binding" }) : "false";
  const onCloseFn = findFnProp(propsArg, "onClose");
  const closeBody = onCloseFn ? handlerBody(onCloseFn, scope) : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}id: ${wrapId}`,
    `${i(1)}cssPrimitive: "dialog"`,
    // Zero-size: the wrapper is a CSS anchor only — the dialog paints on the Overlay.
    `${i(1)}width: 0`,
    `${i(1)}height: 0`,
    `${i(1)}implicitWidth: 0`,
    `${i(1)}implicitHeight: 0`,
    `${i(1)}T.Dialog {`,
    `${i(2)}id: ${dlgId}`,
    `${i(2)}modal: true`,
    `${i(2)}visible: ${openExpr}`,
    // Modal scrim: the default style provides none, so the dialog floats with the page fully
    // visible behind it — it reads as a plain div, not a modal. A semi-transparent dim behind is
    // THE visual that makes it a dialog (same fix as <Drawer>). Author can override via `.dialog`.
    `${i(2)}T.Overlay.modal: Rectangle { color: "#66000000" }`,
    // Center on the window: popups position relative to their parent item — the Overlay.
    `${i(2)}parent: T.Overlay.overlay`,
    `${i(2)}x: Math.round((parent.width - width) / 2)`,
    `${i(2)}y: Math.round((parent.height - height) / 2)`,
    `${i(2)}implicitWidth: contentWidth + leftPadding + rightPadding`,
    `${i(2)}implicitHeight: contentHeight + topPadding + bottomPadding`,
    `${i(2)}padding: 1`,
    ...(closeBody ? [`${i(2)}onClosed: { ${closeBody} }`] : []),
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}cssPrimitive: "div"`,
    `${i(3)}cssClass: ["popup"]`,
    `${i(2)}}`,
    // contentItem: plain Item host (children keep their own Css layout); implicit size from
    // childrenRect so contentWidth/Height see the author's root element.
    `${i(2)}contentItem: Item {`,
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    `${i(3)}implicitWidth: childrenRect.width`,
    `${i(3)}implicitHeight: childrenRect.height`,
    ...emitChildren(children, scope, level + 3),
    `${i(2)}}`,
    `${i(1)}}`,
    // Controlled open state: survives the imperative visible=false a self-close performs.
    `${i(1)}Binding {`,
    `${i(2)}target: ${dlgId}`,
    `${i(2)}property: "visible"`,
    `${i(2)}value: ${openExpr}`,
    `${i(2)}restoreMode: Binding.RestoreNone`,
    `${i(1)}}`,
    `${pad}}`,
  ];
  return lines;
}

// ─────────────────────────────────────────────────────────────────────────────────────────
// <details open> + <summary> → disclosure expander
// ─────────────────────────────────────────────────────────────────────────────────────────

/** <details open={expr}><summary>Label</summary>children</details> → expander.
 *
 *  Wrapper Css.CssFill (cssPrimitive "details") owns `property bool __open`, initialized from
 *  the `open` prop when present (the init binding breaks on the first user toggle — standard
 *  QML semantics, same as the calendar's nav month). cssState carries "open" for author CSS.
 *
 *  The first <summary> child renders as a header row: Css.CssFill (cssPrimitive "summary")
 *  with a "▸" marker glyph, the summary's own children, and a MouseArea that toggles __open
 *  (doubling as the hover tracker, like the button emitter). The marker is a plain Text —
 *  anchored plain internals of a Css container MUST live inside an anchors.fill Item host:
 *  once the author gives the container box rules the engine's flex pass hits plain children
 *  too (see the checkbox indicator emitter). The nested CssItem injects `.marker` CSS
 *  (color/font) without joining any layout. Give the summary padding-left in CSS to clear it.
 *
 *  Remaining children render inside a content Css.CssRect (cssClass ["content"]) visible only
 *  while open — the layout engine skips invisible items, so closing reclaims the space. */
function emitDetails(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));

  const counter = scope.inputCounter ?? { n: 0 };
  const detId = `__details${counter.n++}`;
  const hoverCounter = scope.hoverCounter ?? { n: 0 };
  const maId = `__hover${hoverCounter.n++}`;

  const openNode = findProp(propsArg, "open");
  const openExpr = openNode ? emitExpr(openNode, { ...scope, mode: "binding" }) : "false";

  // Split out the first <summary> child; everything else is disclosure content.
  const summaryIdx = children.findIndex((c) => hTag(c) === "summary");
  const summaryCall = summaryIdx >= 0 ? (children[summaryIdx] as t.CallExpression) : null;
  const rest = children.filter((_, k) => k !== summaryIdx);
  const summaryProps = readBaseProps(summaryCall ? hParts(summaryCall).props : undefined);
  const summaryKids = summaryCall ? hParts(summaryCall).children : [];
  const summaryClassLine = buildCssClassLine(summaryProps, scope, i(2));

  const summaryState = `(${detId}.__open ? ["open"] : []).concat(${maId}.containsMouse ? ["hover"] : [])`;

  return [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}id: ${detId}`,
    `${i(1)}cssPrimitive: "details"`,
    `${i(1)}cssState: ${detId}.__open ? ["open"] : []`,
    // Disclosure state — init binding from `open` breaks on the first click (QML semantics).
    `${i(1)}property bool __open: !!(${openExpr})`,
    // ── summary header row ────────────────────────────────────────────────
    `${i(1)}Css.CssFill {`,
    ...summaryClassLine,
    `${i(2)}cssPrimitive: "summary"`,
    `${i(2)}cssState: ${summaryState}`,
    // Marker glyph: plain Text inside an anchors.fill Item host (flex-pass insulation —
    // see jsdoc); rotates 90° while open. `.marker` CSS lands via the nested CssItem.
    `${i(2)}Item {`,
    `${i(3)}anchors.fill: parent`,
    `${i(3)}Text {`,
    `${i(4)}text: "▸"`,
    `${i(4)}rotation: ${detId}.__open ? 90 : 0`,
    `${i(4)}anchors.left: parent.left`,
    `${i(4)}anchors.leftMargin: 6`,
    `${i(4)}anchors.verticalCenter: parent.verticalCenter`,
    `${i(4)}Css.CssItem { cssPrimitive: "text"; cssClass: ["marker"] }`,
    `${i(3)}}`,
    `${i(2)}}`,
    ...emitChildren(summaryKids, scope, level + 2),
    `${i(2)}MouseArea {`,
    `${i(3)}id: ${maId}`,
    `${i(3)}anchors.fill: parent`,
    `${i(3)}hoverEnabled: true`,
    `${i(3)}cursorShape: Qt.PointingHandCursor`,
    `${i(3)}onClicked: ${detId}.__open = !${detId}.__open`,
    `${i(2)}}`,
    `${i(1)}}`,
    // ── disclosure content: only while open (invisible items leave the layout) ──
    `${i(1)}Css.CssRect {`,
    `${i(2)}cssPrimitive: "div"`,
    `${i(2)}cssClass: ["content"]`,
    `${i(2)}visible: ${detId}.__open`,
    ...emitChildren(rest, scope, level + 2),
    `${i(1)}}`,
    `${pad}}`,
  ];
}

registerNativeTags({
  progress: emitProgress,
  fieldset: emitFieldset,
  legend: emitLegend,
  dialog: emitDialog,
  details: emitDetails,
});
