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
import { registerNativeTags, requireImport } from "./index.ts";
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
  // One .qml per component: instantiate W.Progress (T.ProgressBar + track/bar live in the .qml).
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const bind: Scope = { ...scope, mode: "binding" };
  const valueNode = findProp(propsArg, "value");
  const valueExpr = valueNode ? emitExpr(valueNode, bind) : null;
  const maxNode = findProp(propsArg, "max");
  const maxExpr = maxNode ? emitExpr(maxNode, bind) : null;

  const lines = [`${pad}W.Progress {`, ...classLine, ...guardLine(guard, level)];
  if (maxExpr !== null) lines.push(`${i(1)}max: ${maxExpr}`);
  // No value → the component's default value (-1) means indeterminate.
  if (valueExpr !== null) lines.push(`${i(1)}value: ${valueExpr}`);
  lines.push(`${pad}}`);
  return lines;
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

/** <dialog open={expr} onClose={fn} title="…">children</dialog> → a REAL top-level Window.
 *
 *  Owner directive (2026-07-05): "dialog é uma janela, não um div. dialog é uma janela." A desktop
 *  dialog is an actual OS window (like QDialog), NOT an in-window overlay popup. This supersedes the
 *  earlier T.Dialog-on-the-Overlay approach: a QtQuick Window with `flags: Qt.Dialog` (dialog frame),
 *  `modality: Qt.WindowModal` (blocks its parent), and `transientParent` pinned to the owning window
 *  so the compositor stacks/centers it as a child dialog.
 *
 *  Structure: a zero-size Item anchor in the page (carries the guard fold + gives `transientParent`
 *  via its `Window.window`); the Window sizes itself to the content (`root.implicitWidth/Height`),
 *  and a Css.CssRect root inside carries the author classes. A SEPARATE window severs the CSS
 *  ancestor chain even harder than an overlay reparent — scoped selectors (`.native .my-dialog …`)
 *  and inherited props would stop flowing — so the root carries `cssAncestor: <wrap>` to re-anchor
 *  the engine's ancestor walk back at the page anchor (which still sits under `.native`).
 *  The CSS engine's context props (cssTheme/cssLayout) resolve in the child window (shared root ctx).
 *
 *  visible: controlled by the author's `open` signal via a RestoreNone Binding (survives a self-close
 *  — the window's own close button / Esc); `onClosing` fires the author's onClose so the signal stays
 *  honest when the user closes the window chrome. */
function emitDialog(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(2));
  requireImport(scope, "import QtQuick.Window");

  const counter = scope.inputCounter ?? { n: 0 };
  const n = counter.n++;
  const wrapId = `__dialog${n}W`;
  const dlgId = `__dialog${n}`;
  const rootId = `__dialog${n}Root`;

  const openNode = findProp(propsArg, "open");
  const openExpr = openNode ? emitExpr(openNode, { ...scope, mode: "binding" }) : "false";
  // Fold a <Show> guard into the window's visibility (a window is shown, not laid out).
  const visibleValue = guard ? `!!(${guard}) && !!(${openExpr})` : `!!(${openExpr})`;
  const onCloseFn = findFnProp(propsArg, "onClose");
  const closeBody = onCloseFn ? handlerBody(onCloseFn, scope) : "";
  const titleNode = findProp(propsArg, "title");
  const titleExpr = titleNode ? emitExpr(titleNode, { ...scope, mode: "binding" }) : '""';

  const lines: string[] = [
    // Zero-size page anchor: provides transientParent (its Window.window) and the guard fold.
    `${pad}Item {`,
    `${i(1)}id: ${wrapId}`,
    `${i(1)}width: 0`,
    `${i(1)}height: 0`,
    `${i(1)}Window {`,
    `${i(2)}id: ${dlgId}`,
    `${i(2)}flags: Qt.Dialog`,
    `${i(2)}modality: Qt.WindowModal`,
    `${i(2)}transientParent: ${wrapId}.Window.window`,
    `${i(2)}title: ${titleExpr}`,
    `${i(2)}visible: ${visibleValue}`,
    // Size the window to its content (the Css root's implicit size). Not circular: the root's
    // implicit is content-driven, its actual size comes back via anchors.fill.
    `${i(2)}width: Math.max(1, ${rootId}.implicitWidth)`,
    `${i(2)}height: Math.max(1, ${rootId}.implicitHeight)`,
    ...(closeBody ? [`${i(2)}onClosing: { ${closeBody} }`] : []),
    `${i(2)}Css.CssRect {`,
    `${i(3)}id: ${rootId}`,
    `${i(3)}anchors.fill: parent`,
    // Re-anchor the CSS ancestor walk at the page wrapper: the dialog lives in a separate window,
    // so without this scoped rules (`.native .dialog …`) and inheritance don't reach it.
    `${i(3)}property Item cssAncestor: ${wrapId}`,
    ...classLine,
    `${i(3)}cssPrimitive: "dialog"`,
    ...emitChildren(children, scope, level + 3),
    `${i(2)}}`,
    `${i(1)}}`,
    // Controlled open state: survives the imperative visible=false a window self-close performs.
    `${i(1)}Binding {`,
    `${i(2)}target: ${dlgId}`,
    `${i(2)}property: "visible"`,
    `${i(2)}value: ${visibleValue}`,
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
