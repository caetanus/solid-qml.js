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

/** <fieldset><legend>…</legend>children</fieldset> → W.Fieldset (a CssFill box, cssPrimitive
 *  "fieldset"). One .qml per component: the wrapper lives in Fieldset.qml; the emit only reorders
 *  children (the FIRST <legend> is spliced to the front so it paints first, as the registered
 *  legend CssText) and passes them into the component's default `data`. The border/box look is
 *  entirely author CSS. */
function emitFieldset(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // Splice the first <legend> child to the front; everything else keeps document order.
  const legendIdx = children.findIndex((c) => hTag(c) === "legend");
  const ordered = legendIdx >= 0
    ? [children[legendIdx], ...children.filter((_, k) => k !== legendIdx)]
    : children;

  return [
    `${pad}W.Fieldset {`,
    ...classLine,
    ...guardLine(guard, level),
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
  const classLine = buildCssClassLine(props, scope, i(1));
  // One .qml per component: instantiate W.Dialog (the page-anchor Item + modal Window + Css root live
  // in Dialog.qml). Consume a counter slot to keep sibling widgets monotonically numbered.
  const counter = scope.inputCounter ?? { n: 0 };
  counter.n++;
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const openNode = findProp(propsArg, "open");
  const openExpr = openNode ? emitExpr(openNode, { ...scope, mode: "binding" }) : "false";
  // Fold a <Show> guard into the window's visibility (a window is shown, not laid out) — passed as
  // the component's `open` prop rather than a guardLine (the page-anchor Item is always in the tree).
  const visibleValue = guard ? `!!(${guard}) && !!(${openExpr})` : `!!(${openExpr})`;
  const onCloseFn = findFnProp(propsArg, "onClose");
  const closeBody = onCloseFn ? handlerBody(onCloseFn, scope) : "";
  const titleNode = findProp(propsArg, "title");
  const titleExpr = titleNode ? emitExpr(titleNode, { ...scope, mode: "binding" }) : null;

  const lines: string[] = [
    `${pad}W.Dialog {`,
    `${i(1)}open: ${visibleValue}`,
    ...(titleExpr !== null ? [`${i(1)}title: ${titleExpr}`] : []),
    ...classLine,
    // onDialogClosed relays the window's onClosing so the author's onClose keeps the signal honest.
    ...(closeBody ? [`${i(1)}onDialogClosed: { ${closeBody} }`] : []),
    ...emitChildren(children, scope, level + 1),
    `${pad}}`,
  ];
  return lines;
}

// ─────────────────────────────────────────────────────────────────────────────────────────
// <details open> + <summary> → disclosure expander
// ─────────────────────────────────────────────────────────────────────────────────────────

/** <details open={expr}><summary>Label</summary>children</details> → W.Details.
 *
 *  One .qml per component: the CssFill "details" wrapper, the `__open` state, the summary header row
 *  (marker glyph + toggle/hover MouseArea) and the content box all live in Details.qml. The emit
 *  only: seeds `open` (when present); passes the summary's author classes as `summaryClass` and the
 *  summary's own children as `summaryContent` (a QML list literal appended after the marker/
 *  MouseArea); and passes the disclosure body via the component's default `content`. */
function emitDetails(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard: string | undefined): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = readBaseProps(propsArg);
  const classLine = buildCssClassLine(props, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  // No value → the component's default open (false). Present → seed the init binding.
  const openNode = findProp(propsArg, "open");
  const openExpr = openNode ? emitExpr(openNode, { ...scope, mode: "binding" }) : null;

  // Split out the first <summary> child; everything else is disclosure content.
  const summaryIdx = children.findIndex((c) => hTag(c) === "summary");
  const summaryCall = summaryIdx >= 0 ? (children[summaryIdx] as t.CallExpression) : null;
  const rest = children.filter((_, k) => k !== summaryIdx);
  const summaryProps = readBaseProps(summaryCall ? hParts(summaryCall).props : undefined);
  const summaryKids = summaryCall ? hParts(summaryCall).children : [];
  // buildCssClassLine yields `cssClass: <array>` — strip the label to reuse the array for summaryClass.
  const summaryClassLine = buildCssClassLine(summaryProps, scope, "");
  const summaryClassArr = summaryClassLine.length ? summaryClassLine[0].replace(/^cssClass:\s*/, "") : null;

  const lines = [`${pad}W.Details {`, ...classLine, ...guardLine(guard, level)];
  if (openExpr !== null) lines.push(`${i(1)}open: ${openExpr}`);
  if (summaryClassArr !== null) lines.push(`${i(1)}summaryClass: ${summaryClassArr}`);
  // Summary's own children as a QML list literal (aliased into the summary box, after marker/MouseArea).
  lines.push(...emitListLiteral("summaryContent", summaryKids, scope, level + 1));
  // Disclosure body → the component's default `content` alias.
  lines.push(...emitChildren(rest, scope, level + 1));
  lines.push(`${pad}}`);
  return lines;
}

/** Emit `name: [ <children> ]` — the children as elements of a QML list literal. Groups adjacent
 *  text/interpolation runs into a single CssText exactly like emitChildren, then comma-joins the
 *  top-level blocks (identified by their closing brace at the base indent). Returns [] when empty. */
function emitListLiteral(name: string, children: t.Node[], scope: Scope, level: number): string[] {
  const inner = level + 1;
  const block = emitChildren(children, scope, inner);
  if (block.length === 0) return [];
  const close = INDENT.repeat(inner) + "}";
  const closeIdxs: number[] = [];
  block.forEach((l, k) => { if (l === close) closeIdxs.push(k); });
  for (let k = 0; k < closeIdxs.length - 1; k++) block[closeIdxs[k]] += ",";
  const pad = INDENT.repeat(level);
  return [`${pad}${name}: [`, ...block, `${pad}]`];
}

registerNativeTags({
  progress: emitProgress,
  fieldset: emitFieldset,
  legend: emitLegend,
  dialog: emitDialog,
  details: emitDetails,
});
