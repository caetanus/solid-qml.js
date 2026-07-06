// Native-only input widgets (plan phases 2–3, inputs group):
//   <RangeSlider>, <Dial>, <Tumbler>, <DelayButton>, <BusyIndicator>,
//   <RoundButton>, <ToolButton>, <ToolSeparator>
//
// Every control follows the house widget idiom (see emitSlider / emitCheckboxToggle in
// ../qml.ts): a wrapper Css.CssFill carries CSS identity (classes, cssState, layout
// participation) and the T.* Templates control fills it with anchors.fill. Template slots
// (background / handle / contentItem / indicator) are filled with Css items; PLAIN internals
// that need anchors/x/y inside a Css container are hosted in an `Item { anchors.fill: parent }`
// — the CSS layout engine skips anchored plain Items but flex-lays-out (or top-left-pins)
// everything else (see the checkbox indicator comment in ../qml.ts for the full story).
//
// Handler convention for these QML-only tags: handlers receive VALUES directly
// (onChange={(lo, hi) => …}, onChange={(v) => …}) — not a synthetic DOM event. These tags
// have no HTML counterpart, so there is no `e.target.value` idiom to preserve.
//
// Templates implicit sizes: T.* controls have implicitWidth/Height 0 — deriving them is the
// STYLE's job (we are the style). Each control root gets the Basic-style formula
// (max of background+insets and content+paddings) so a bare widget has a sane natural size;
// authors override via CSS on the wrapper as with every other widget.
import * as t from "@babel/types";
import { registerNativeTags, type NativeEmit } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { buildCssClassLine, guardLine, INDENT } from "../qml.ts";
import { safeName } from "../../names/safe.ts";
import { isHCall } from "../../ast/h.ts";

type Fn = t.ArrowFunctionExpression | t.FunctionExpression;

/** Props shape accepted by buildCssClassLine (the Props interface is module-local to qml.ts). */
type UiProps = Parameters<typeof buildCssClassLine>[0];

// ── prop readers ─────────────────────────────────────────────────────────────────────────

function propValue(propsArg: t.Node | undefined, name: string): t.Expression | undefined {
  if (!propsArg || !t.isObjectExpression(propsArg)) return undefined;
  for (const p of propsArg.properties) {
    if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name }) && t.isExpression(p.value)) return p.value;
  }
  return undefined;
}

/** Emit a prop's expression in binding mode, or undefined when absent. */
function bindingExpr(propsArg: t.Node | undefined, name: string, scope: Scope): string | undefined {
  const e = propValue(propsArg, name);
  return e ? emitExpr(e, { ...scope, mode: "binding" }) : undefined;
}

/** Numeric attribute (min/max/step/delay): literal → literal text, else binding expression. */
function numericAttr(propsArg: t.Node | undefined, name: string, dflt: string, scope: Scope): string {
  const e = propValue(propsArg, name);
  if (!e) return dflt;
  if (t.isNumericLiteral(e)) return String(e.value);
  if (t.isStringLiteral(e)) return e.value;
  return emitExpr(e, { ...scope, mode: "binding" });
}

function fnProp(propsArg: t.Node | undefined, name: string): Fn | null {
  const e = propValue(propsArg, name);
  return e && (t.isArrowFunctionExpression(e) || t.isFunctionExpression(e)) ? e : null;
}

/** `disabled` / bare boolean attribute — present and not literally false. */
function boolAttr(propsArg: t.Node | undefined, name: string): boolean {
  if (!propsArg || !t.isObjectExpression(propsArg)) return false;
  for (const p of propsArg.properties) {
    if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name }))
      return !t.isBooleanLiteral(p.value) || p.value.value;
  }
  return false;
}

/** class / classList subset of qml.ts readProps (readProps itself is module-local). */
function uiProps(propsArg: t.Node | undefined): UiProps {
  const props: UiProps = { classes: [], classList: [], onClick: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
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
  }
  return props;
}

// ── handler translation ──────────────────────────────────────────────────────────────────

/** Emit an arrow/function body as QML-JS statements (same joiner as translateValueHandler). */
function fnBody(fn: Fn, inner: Scope): string {
  if (t.isBlockStatement(fn.body)) {
    return fn.body.body.map((s) => {
      if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
      if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
      return "";
    }).join(" ");
  }
  return emitExpr(fn.body, inner);
}

/** Direct-value handler: positional params are aliased to the given QML expressions.
 *  `onChange={(lo, hi) => …}` with ["ctl.first.value", "ctl.second.value"] rewrites lo/hi. */
function translateArgsHandler(fn: Fn, argExprs: string[], scope: Scope): string {
  const locals: Record<string, string> = { ...(scope.locals ?? {}) };
  fn.params.forEach((p, idx) => {
    if (t.isIdentifier(p) && argExprs[idx]) locals[p.name] = argExprs[idx];
  });
  return fnBody(fn, { ...scope, mode: "handler", locals, selfId: "__self" });
}

/** Zero/value-arg event handler: inline arrows (expr or block body) or a local helper name. */
function emitEventHandler(node: t.Node | undefined, scope: Scope, argExprs: string[] = []): string | null {
  if (!node) return null;
  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) return translateArgsHandler(node, argExprs, scope);
  if (t.isIdentifier(node) && scope.helpers?.has(node.name)) return `${safeName(node.name)}()`;
  throw new Error("only inline arrow handlers (or a local helper reference) are supported for native widget events");
}

// ── shared emission bits ─────────────────────────────────────────────────────────────────

/** Allocate the control id and mark the Templates import (same idiom as emitInput). */
function allocCtl(scope: Scope): string {
  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;
  if (scope.usedWidgets) scope.usedWidgets.flag = true;
  return ctlId;
}

/** String-valued binding from text/interpolation children (local clone of qml.ts textBinding). */
function textLabel(children: t.Node[], scope: Scope): string {
  const parts: string[] = [];
  for (const child of children) {
    if (isHCall(child)) continue; // element children carry no label text
    if (t.isStringLiteral(child)) {
      if (child.value.trim() || /\s/.test(child.value)) parts.push(JSON.stringify(child.value));
    } else if (child.type === "JSXText") {
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

/** Binding element re-asserting a controlled value (emitInput's idiom, RestoreNone). */
function bindingElement(i: (n: number) => string, target: string, property: string, value: string): string[] {
  return [
    `${i(1)}Binding {`,
    `${i(2)}target: ${target}`,
    `${i(2)}property: ${JSON.stringify(property)}`,
    `${i(2)}value: ${value}`,
    `${i(2)}restoreMode: Binding.RestoreNone`,
    `${i(1)}}`,
  ];
}

/** The focused-wheel accumulator from emitSlider (Mouse|TouchPad, 120-unit notches), stepping
 *  the given value holder (`ctl` for Slider-likes, `ctl.first` for the RangeSlider) and
 *  re-firing its moved() so the author's onChange wiring runs. */
function wheelStepper(i: (n: number) => string, ctlId: string, node: string): string[] {
  return [
    `${i(2)}WheelHandler {`,
    `${i(3)}property real __acc: 0`,
    `${i(3)}enabled: ${ctlId}.activeFocus`,
    `${i(3)}acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad`,
    `${i(3)}onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ${node}.value = Math.max(${ctlId}.from, Math.min(${ctlId}.to, ${node}.value + s * ${ctlId}.stepSize)); ${node}.moved() } }`,
    `${i(2)}}`,
  ];
}

/** Basic-style implicit size formula: templates leave implicit sizes to the style (us). */
function implicitFormula(i: (n: number) => string): string[] {
  return [
    `${i(2)}implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)`,
    `${i(2)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)`,
  ];
}

// ── <RangeSlider min max step first={a()} second={b()} onChange={(lo,hi)=>…}> ─────────────
//
// T.RangeSlider has NO value/handle of its own: `first` and `second` are sub-objects
// (QQuickRangeSliderNode, qquickrangeslider_p.h) each carrying value / visualPosition /
// handle / moved(). Track and handle geometry copy emitSlider; the range fill spans
// [first.visualPosition, second.visualPosition]. The fill lives in an anchored Item host:
// unlike the Slider's zero-based fill, its x offset must survive the CSS flex pass that
// runs over the track's Css children once `.track` carries box rules.
const emitRangeSlider: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  const ctlId = allocCtl(scope);

  const min = numericAttr(propsArg, "min", "0", scope);
  const max = numericAttr(propsArg, "max", "100", scope);
  const step = numericAttr(propsArg, "step", "1", scope);
  const firstExpr = bindingExpr(propsArg, "first", scope);
  const secondExpr = bindingExpr(propsArg, "second", scope);
  const onChangeFn = fnProp(propsArg, "onChange");
  const disabled = boolAttr(propsArg, "disabled");

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  // onChange(lo, hi) — direct values, fired from BOTH nodes' moved() signals.
  const changeBody = onChangeFn
    ? translateArgsHandler(onChangeFn, [`${ctlId}.first.value`, `${ctlId}.second.value`], scope)
    : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: "input"`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.RangeSlider {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    `${i(2)}from: ${min}`,
    `${i(2)}to: ${max}`,
    `${i(2)}stepSize: ${step}`,
    // Style-side implicit size: background/handles carry the natural metrics (Basic idiom).
    `${i(2)}implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, first.implicitHandleWidth + leftPadding + rightPadding)`,
    `${i(2)}implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, first.implicitHandleHeight + topPadding + bottomPadding)`,
    // Track: same geometry as emitSlider (6px tall, centred within the control).
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["track"]`,
    `${i(3)}x: ${ctlId}.leftPadding`,
    `${i(3)}y: ${ctlId}.topPadding + (${ctlId}.availableHeight - height) / 2`,
    `${i(3)}width: ${ctlId}.availableWidth`,
    `${i(3)}height: 6`,
    `${i(3)}implicitWidth: 200`,
    `${i(3)}implicitHeight: 6`,
    // Anchored Item host: the range fill starts at first.visualPosition (x ≠ 0) — a bare Css
    // child would be re-laid-out to x 0 by the CSS flex pass over the styled track.
    `${i(3)}Item {`,
    `${i(4)}anchors.fill: parent`,
    `${i(4)}Css.CssRect {`,
    `${i(5)}cssClass: ["track-fill"]`,
    `${i(5)}x: ${ctlId}.first.visualPosition * parent.width`,
    `${i(5)}width: (${ctlId}.second.visualPosition - ${ctlId}.first.visualPosition) * parent.width`,
    `${i(5)}height: parent.height`,
    `${i(4)}}`,
    `${i(3)}}`,
    `${i(2)}}`,
    // Handles: emitSlider's handle geometry, one per node, positioned by the node's own
    // visualPosition. No Behavior — dragging must be 1:1.
    `${i(2)}first.handle: Css.CssRect {`,
    `${i(3)}cssClass: ["handle"]`,
    `${i(3)}width: 18`,
    `${i(3)}height: 18`,
    `${i(3)}implicitWidth: 18`,
    `${i(3)}implicitHeight: 18`,
    `${i(3)}x: ${ctlId}.leftPadding + ${ctlId}.first.visualPosition * (${ctlId}.availableWidth - width)`,
    `${i(3)}y: ${ctlId}.topPadding + ${ctlId}.availableHeight / 2 - height / 2`,
    `${i(2)}}`,
    `${i(2)}second.handle: Css.CssRect {`,
    `${i(3)}cssClass: ["handle"]`,
    `${i(3)}width: 18`,
    `${i(3)}height: 18`,
    `${i(3)}implicitWidth: 18`,
    `${i(3)}implicitHeight: 18`,
    `${i(3)}x: ${ctlId}.leftPadding + ${ctlId}.second.visualPosition * (${ctlId}.availableWidth - width)`,
    `${i(3)}y: ${ctlId}.topPadding + ${ctlId}.availableHeight / 2 - height / 2`,
    `${i(2)}}`,
    // Focused wheel steps the FIRST handle (emitSlider's accumulator, re-fires first.moved()).
    ...wheelStepper(i, ctlId, `${ctlId}.first`),
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (changeBody) {
    lines.push(`${i(2)}first.onMoved: { ${changeBody} }`);
    lines.push(`${i(2)}second.onMoved: { ${changeBody} }`);
  }
  lines.push(`${i(1)}}`);

  // Controlled values: one Binding per node — Binding.target accepts the sub-object directly.
  if (firstExpr !== undefined) lines.push(...bindingElement(i, `${ctlId}.first`, "value", firstExpr));
  if (secondExpr !== undefined) lines.push(...bindingElement(i, `${ctlId}.second`, "value", secondExpr));

  lines.push(`${pad}}`);
  return lines;
};

// ── <Dial value min max step onChange={(v)=>…}> ───────────────────────────────────────────
//
// T.Dial positions NOTHING (qquickdial_p.h): the STYLE places the handle from `angle`
// (0° = 12 o'clock, positive clockwise — the Basic style rotates its handle image by the
// same angle). We compute the point directly: cx + sin(angle)·r, cy − cos(angle)·r with
// r = background.width/2 − 12. The dial circle is the background slot (border-radius via CSS).
const emitDial: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  const ctlId = allocCtl(scope);

  const min = numericAttr(propsArg, "min", "0", scope);
  const max = numericAttr(propsArg, "max", "100", scope);
  const step = numericAttr(propsArg, "step", "1", scope);
  const valueExpr = bindingExpr(propsArg, "value", scope);
  const onChangeFn = fnProp(propsArg, "onChange");
  const disabled = boolAttr(propsArg, "disabled");

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(${ctlId}.pressed ? ["active"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  const movedBody = onChangeFn ? translateArgsHandler(onChangeFn, [`${ctlId}.value`], scope) : "";
  const radius = `(${ctlId}.background.width / 2 - 12)`;

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: ""`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.Dial {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    `${i(2)}from: ${min}`,
    `${i(2)}to: ${max}`,
    `${i(2)}stepSize: ${step}`,
    ...implicitFormula(i),
    // The dial face: a centred square CssFill — `.dial { border-radius: … }` makes it a circle.
    `${i(2)}background: Css.CssFill {`,
    `${i(3)}cssPrimitive: ""`,
    `${i(3)}cssClass: ["dial"]`,
    `${i(3)}x: ${ctlId}.width / 2 - width / 2`,
    `${i(3)}y: ${ctlId}.height / 2 - height / 2`,
    `${i(3)}width: Math.max(32, Math.min(${ctlId}.width, ${ctlId}.height))`,
    `${i(3)}height: width`,
    `${i(3)}implicitWidth: 96`,
    `${i(3)}implicitHeight: 96`,
    `${i(2)}}`,
    `${i(2)}handle: Css.CssRect {`,
    `${i(3)}cssClass: ["handle"]`,
    `${i(3)}width: 12`,
    `${i(3)}height: 12`,
    `${i(3)}implicitWidth: 12`,
    `${i(3)}implicitHeight: 12`,
    `${i(3)}x: ${ctlId}.background.x + ${ctlId}.background.width / 2 - width / 2 + Math.sin(${ctlId}.angle * Math.PI / 180) * ${radius}`,
    `${i(3)}y: ${ctlId}.background.y + ${ctlId}.background.height / 2 - height / 2 - Math.cos(${ctlId}.angle * Math.PI / 180) * ${radius}`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  if (movedBody) lines.push(`${i(2)}onMoved: { ${movedBody} }`);
  lines.push(`${i(1)}}`);

  if (valueExpr !== undefined) lines.push(...bindingElement(i, ctlId, "value", valueExpr));

  lines.push(`${pad}}`);
  return lines;
};

// ── <Tumbler options={[...]} value onChange={(v)=>…}> ─────────────────────────────────────
//
// T.Tumbler instantiates NO view of its own (qquicktumbler_p.h): the C++ walks contentItem
// looking for a PathView or ListView (determineViewType). The Basic style uses a private
// TumblerView helper (PathView-backed) we cannot import, so our contentItem is the minimal
// working ListView: SnapToItem + StrictlyEnforceRange with a one-item-tall preferred
// highlight window centred in the view — the exact non-wrap configuration TumblerView
// generates internally. `wrap: false` is EXPLICIT: with count ≥ visibleItemCount the
// implicit default flips to wrapping, and the C++ then expects a PathView.
//
// The attached Tumbler.displacement must be read on the delegate ROOT (the attached object's
// init requires a delegate item with a parent and the `index` context property); the root
// Item mirrors it into `__disp` for the inner CssText's cssState. Cell size is bound
// declaratively (availableWidth × availableHeight/visibleItemCount — the template's own
// resize formula): the C++ imperative resize misses items incubated after the last geometry
// change (same lesson as the calendar grid delegates).
const emitTumbler: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  const ctlId = allocCtl(scope);

  const optionsExpr = bindingExpr(propsArg, "options", scope) ?? "[]";
  const valueExpr = bindingExpr(propsArg, "value", scope);
  const onChangeFn = fnProp(propsArg, "onChange");
  const disabled = boolAttr(propsArg, "disabled");

  const cssState = `(${ctlId}.activeFocus ? ["focus"] : []).concat(!${ctlId}.enabled ? ["disabled"] : [])`;
  // Direct-value handler: the picked option is model[currentIndex].
  const changeBody = onChangeFn
    ? translateArgsHandler(onChangeFn, [`${ctlId}.model[${ctlId}.currentIndex]`], scope)
    : "";

  const lines: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}cssPrimitive: ""`,
    `${i(1)}cssState: ${cssState}`,
    `${i(1)}implicitWidth: ${ctlId}.implicitWidth`,
    `${i(1)}implicitHeight: ${ctlId}.implicitHeight`,
    `${i(1)}T.Tumbler {`,
    `${i(2)}id: ${ctlId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}activeFocusOnTab: solidTabstop.enabled`,
    `${i(2)}model: ${optionsExpr}`,
    `${i(2)}wrap: false`,
    ...implicitFormula(i),
    `${i(2)}delegate: Item {`,
    `${i(3)}width: ${ctlId}.availableWidth`,
    `${i(3)}height: ${ctlId}.availableHeight / ${ctlId}.visibleItemCount`,
    `${i(3)}property real __disp: T.Tumbler.displacement`,
    `${i(3)}Css.CssText {`,
    `${i(4)}cssPrimitive: ""`,
    `${i(4)}cssClass: ["item"]`,
    // displacement is 0 for the row settled on the centre; the view snaps, but the settle
    // is float-valued — a half-row tolerance marks exactly one row selected.
    `${i(4)}cssState: Math.abs(__disp) < 0.5 ? ["selected"] : []`,
    `${i(4)}text: modelData`,
    `${i(4)}anchors.centerIn: parent`,
    `${i(3)}}`,
    `${i(2)}}`,
    `${i(2)}contentItem: ListView {`,
    `${i(3)}implicitWidth: 60`,
    `${i(3)}implicitHeight: 180`,
    `${i(3)}model: ${ctlId}.model`,
    `${i(3)}delegate: ${ctlId}.delegate`,
    `${i(3)}snapMode: ListView.SnapToItem`,
    `${i(3)}highlightRangeMode: ListView.StrictlyEnforceRange`,
    `${i(3)}preferredHighlightBegin: height / 2 - height / ${ctlId}.visibleItemCount / 2`,
    `${i(3)}preferredHighlightEnd: height / 2 + height / ${ctlId}.visibleItemCount / 2`,
    `${i(3)}clip: true`,
    `${i(2)}}`,
  ];

  if (disabled) lines.push(`${i(2)}enabled: false`);
  // Fires for user flicks AND Binding re-assertions — the echo writes the same value back
  // into the signal, which is a no-op (same acceptance as the <select> onActivated wiring).
  if (changeBody) lines.push(`${i(2)}onCurrentIndexChanged: { ${changeBody} }`);
  lines.push(`${i(1)}}`);

  if (valueExpr !== undefined)
    lines.push(...bindingElement(i, ctlId, "currentIndex", `(${optionsExpr}).indexOf(${valueExpr})`));

  lines.push(`${pad}}`);
  return lines;
};

// ── <DelayButton delay={ms} onActivated={…}>label</DelayButton> ───────────────────────────
//
// One .qml per component: the paint-less wrapper, the T.DelayButton, the `.delay` pill background
// with its progress overlay and the label CssText all live in DelayButton.qml. The emit only
// instantiates the component and wires delay / text / onActivated / disabled.
const emitDelayButton: NativeEmit = (propsArg, children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const delay = numericAttr(propsArg, "delay", "300", scope);
  const activatedBody = emitEventHandler(propValue(propsArg, "onActivated"), scope);
  const disabled = boolAttr(propsArg, "disabled");

  const lines: string[] = [
    `${pad}W.DelayButton {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}delay: ${delay}`,
    `${i(1)}text: ${textLabel(children, scope)}`,
  ];
  if (disabled) lines.push(`${i(1)}disabled: true`);
  if (activatedBody) lines.push(`${i(1)}onActivated: { ${activatedBody} }`);
  lines.push(`${pad}}`);
  return lines;
};

// ── <BusyIndicator running={bool}> ────────────────────────────────────────────────────────
//
// One .qml per component: the T.BusyIndicator + the eight spinning spokes live in
// BusyIndicator.qml. The emit only instantiates the component and forwards `running`
// (absent → the component default `true`).
const emitBusyIndicator: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  const runningExpr = bindingExpr(propsArg, "running", scope);

  const lines = [`${pad}W.BusyIndicator {`, ...classLine, ...guardLine(guard, level)];
  if (runningExpr !== undefined) lines.push(`${i(1)}running: ${runningExpr}`);
  lines.push(`${pad}}`);
  return lines;
};

// ── <RoundButton> / <ToolButton> ──────────────────────────────────────────────────────────
//
// One .qml per component: the wrapper CssFill (cssPrimitive "button"), the CssText label and the
// T.RoundButton / T.ToolButton filling it all live in RoundButton.qml / ToolButton.qml. The emit
// only instantiates the component, appends the extra class the spec assigns ("round"/"tool") to
// the author classes, and wires text / onClicked / disabled.
function buttonLike(wType: string, extraClass: string): NativeEmit {
  return (propsArg, children, scope, level, guard) => {
    const pad = INDENT.repeat(level);
    const i = (n: number) => INDENT.repeat(level + n);
    const ui = uiProps(propsArg);
    ui.classes = [...ui.classes, extraClass];
    const classLine = buildCssClassLine(ui, scope, i(1));
    if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

    const clickBody = emitEventHandler(ui.onClick, scope);
    const disabled = boolAttr(propsArg, "disabled");

    const lines: string[] = [
      `${pad}${wType} {`,
      ...classLine,
      ...guardLine(guard, level),
      `${i(1)}text: ${textLabel(children, scope)}`,
    ];
    if (disabled) lines.push(`${i(1)}disabled: true`);
    if (clickBody) lines.push(`${i(1)}onClicked: { ${clickBody} }`);
    lines.push(`${pad}}`);
    return lines;
  };
}

// ── <ToolSeparator> ───────────────────────────────────────────────────────────────────────
//
// One .qml per component: the T.ToolSeparator + centred "sep" CssRect live in ToolSeparator.qml.
// The emit only instantiates the component and wires the author classes.
const emitToolSeparator: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;

  return [
    `${pad}W.ToolSeparator {`,
    ...classLine,
    ...guardLine(guard, level),
    `${pad}}`,
  ];
};

registerNativeTags({
  RangeSlider: emitRangeSlider,
  Dial: emitDial,
  Tumbler: emitTumbler,
  DelayButton: emitDelayButton,
  BusyIndicator: emitBusyIndicator,
  RoundButton: buttonLike("W.RoundButton", "round"),
  ToolButton: buttonLike("W.ToolButton", "tool"),
  ToolSeparator: emitToolSeparator,
});
