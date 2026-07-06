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

/** Allocate the control id and mark the Templates import (same idiom as emitInput).
 *  Used by the still-inline emitters (RangeSlider/Tumbler); migrated widgets use allocInstance. */
function allocCtl(scope: Scope): string {
  const counter = scope.inputCounter ?? { n: 0 };
  const ctlId = `__input${counter.n++}`;
  if (scope.usedWidgets) scope.usedWidgets.flag = true;
  return ctlId;
}

/** Allocate the W.<Name> instance id and mark the solidqml.Widgets import. The instance keeps the
 *  `__inputN` name so author handlers that read the control value (`__inputN.value`) and controlled
 *  Bindings resolve against the component's exposed properties/aliases. */
function allocInstance(scope: Scope): string {
  const counter = scope.inputCounter ?? { n: 0 };
  const id = `__input${counter.n++}`;
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
  return id;
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
// One .qml per component: the T.Dial + dial face + trig-placed handle live in Dial.qml. The emit
// instantiates W.Dial (id kept as __inputN so the onMoved value-read `__inputN.value` and the
// controlled Binding resolve against the component's two-way `value` alias) and wires
// from/to/stepSize + the onChange handler + the controlled value.
const emitDial: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  const ctlId = allocInstance(scope);

  const min = numericAttr(propsArg, "min", "0", scope);
  const max = numericAttr(propsArg, "max", "100", scope);
  const step = numericAttr(propsArg, "step", "1", scope);
  const valueExpr = bindingExpr(propsArg, "value", scope);
  const onChangeFn = fnProp(propsArg, "onChange");
  const disabled = boolAttr(propsArg, "disabled");

  // onChange(v) — direct value, fired from the component's moved() signal (control drag / wheel).
  const movedBody = onChangeFn ? translateArgsHandler(onChangeFn, [`${ctlId}.value`], scope) : "";

  const lines: string[] = [
    `${pad}W.Dial {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}from: ${min}`,
    `${i(1)}to: ${max}`,
    `${i(1)}stepSize: ${step}`,
  ];
  if (disabled) lines.push(`${i(1)}disabled: true`);
  if (movedBody) lines.push(`${i(1)}onMoved: { ${movedBody} }`);
  if (valueExpr !== undefined) lines.push(...bindingElement(i, ctlId, "value", valueExpr));
  lines.push(`${pad}}`);
  return lines;
};

// ── <Tumbler options={[...]} value onChange={(v)=>…}> ─────────────────────────────────────
//
// One .qml per component: the T.Tumbler + minimal non-wrap ListView + delegate live in Tumbler.qml.
// The emit instantiates W.Tumbler (id kept as __inputN so the onChange value-read
// `__inputN.model[__inputN.currentIndex]` and the controlled Binding resolve against the
// component's `model`/`currentIndex` aliases) and wires model + onChange + the controlled value.
const emitTumbler: NativeEmit = (propsArg, _children, scope, level, guard) => {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const ui = uiProps(propsArg);
  const classLine = buildCssClassLine(ui, scope, i(1));
  const ctlId = allocInstance(scope);

  const optionsExpr = bindingExpr(propsArg, "options", scope) ?? "[]";
  const valueExpr = bindingExpr(propsArg, "value", scope);
  const onChangeFn = fnProp(propsArg, "onChange");
  const disabled = boolAttr(propsArg, "disabled");

  // Direct-value handler: the picked option is model[currentIndex].
  const changeBody = onChangeFn
    ? translateArgsHandler(onChangeFn, [`${ctlId}.model[${ctlId}.currentIndex]`], scope)
    : "";

  const lines: string[] = [
    `${pad}W.Tumbler {`,
    `${i(1)}id: ${ctlId}`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}model: ${optionsExpr}`,
  ];
  if (disabled) lines.push(`${i(1)}disabled: true`);
  // Fires for user flicks AND Binding re-assertions — the echo writes the same value back
  // into the signal, which is a no-op (same acceptance as the <select> onActivated wiring).
  if (changeBody) lines.push(`${i(1)}onCurrentIndexChanged: { ${changeBody} }`);
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
