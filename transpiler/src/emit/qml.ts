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

  if (t.isIdentifier(tagArg)) {
    if (tagArg.name === "Show") return emitShow(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "For") return emitFor(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Index") return emitIndex(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Switch") return emitSwitch(children as t.Node[], scope, level, guard);
    if (tagArg.name === "Dynamic") return emitDynamic(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Suspense") return emitSuspense(propsArg, children as t.Node[], scope, level, guard);
    if (tagArg.name === "Window") return emitWindow(propsArg, children as t.Node[], scope, level);
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
 *  - `e.currentTarget.value` / `e.target.value` → `text` (the TextInput's text property)
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

/** <input class="x" value={s()} onInput={(e)=>setS(e.currentTarget.value)} onKeyDown={...} placeholder="..." />
 *  → Css.CssFill { TextInput { id: __inputN; text bound; onTextEdited; Keys.onPressed; placeholder Text } Connections } */
function emitInput(propsArg: t.Node | undefined, props: Props, scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const classLine = buildCssClassLine(props, scope, i(1));

  // Allocate a unique id for this TextInput via the per-file counter in scope.
  const counter = scope.inputCounter ?? { n: 0 };
  const inpId = `__input${counter.n++}`;
  // If this is the first (only) input and no counter was in scope, we've mutated the local object —
  // but since it's not threaded back, we only need the id here. For multiple inputs, callers thread
  // the counter via scope.inputCounter.

  // Resolve value, onInput, onKeyDown, placeholder from propsArg.
  let valueExpr = '""';
  let signalName: string | null = null; // the signal name if value={sig()} — for two-way sync
  let onInputFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let onKeyDownFn: (t.ArrowFunctionExpression | t.FunctionExpression) | null = null;
  let placeholder = "";

  if (propsArg && t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
      const key = p.key.name;
      if (key === "value" && t.isExpression(p.value)) {
        valueExpr = emitExpr(p.value, { ...scope, mode: "binding" });
        // Detect value={sig()} — a zero-arg call to a signal accessor.
        if (t.isCallExpression(p.value) && t.isIdentifier(p.value.callee) && p.value.arguments.length === 0) {
          const sym = scope.table.get(p.value.callee.name);
          if (sym?.kind === "signal") signalName = sym.name;
        }
      }
      if (key === "onInput" && t.isExpression(p.value) && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onInputFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if ((key === "onKeyDown" || key === "onKeydown") && t.isExpression(p.value) && (t.isArrowFunctionExpression(p.value) || t.isFunctionExpression(p.value)))
        onKeyDownFn = p.value as t.ArrowFunctionExpression | t.FunctionExpression;
      if (key === "placeholder" && t.isStringLiteral(p.value))
        placeholder = p.value.value;
    }
  }

  // Compute onTextEdited body.
  let inputBody = "";
  if (onInputFn) {
    inputBody = translateInputHandler(onInputFn, scope);
  } else if (signalName) {
    // No explicit onInput but bound to a signal → write-back: sig = text
    inputBody = `${safeName(signalName)} = text`;
  }

  // Compute Keys.onPressed body.
  let keyBody = "";
  if (onKeyDownFn) {
    keyBody = translateInputHandler(onKeyDownFn, scope);
  }

  const parts: string[] = [
    `${pad}Css.CssFill {`,
    ...classLine,
    ...guardLine(guard, level),
    `${i(1)}TextInput {`,
    `${i(2)}id: ${inpId}`,
    `${i(2)}anchors.fill: parent`,
    `${i(2)}anchors.leftMargin: 12`,
    `${i(2)}anchors.rightMargin: 12`,
    `${i(2)}verticalAlignment: TextInput.AlignVCenter`,
    `${i(2)}clip: true`,
    `${i(2)}selectByMouse: true`,
    `${i(2)}activeFocusOnTab: true`,
    `${i(2)}color: parent.style && parent.style["color"] ? cssTheme.parseColor(parent.style["color"]) : "#2b2b2b"`,
    `${i(2)}font.family: parent.style && parent.style["font-family"] ? cssTheme.resolveFontFamily(parent.style["font-family"], "Sans Serif") : cssTheme.resolveFontFamily("Sans Serif")`,
    `${i(2)}font.pointSize: parent.style && parent.style["font-size"] ? cssTheme.parseFontSize(parent.style["font-size"], 13) : 13`,
    `${i(2)}Component.onCompleted: text = ${valueExpr}`,
  ];
  if (inputBody) parts.push(`${i(2)}onTextEdited: { ${inputBody} }`);
  if (keyBody) parts.push(`${i(2)}Keys.onPressed: (event) => { ${keyBody} }`);
  if (placeholder) {
    parts.push(
      `${i(2)}Text {`,
      `${i(3)}anchors.verticalCenter: parent.verticalCenter`,
      `${i(3)}anchors.left: parent.left`,
      `${i(3)}visible: parent.text.length === 0`,
      `${i(3)}text: ${JSON.stringify(placeholder)}`,
      `${i(3)}color: "#9aa0a6"`,
      `${i(3)}font: parent.font`,
      `${i(2)}}`,
    );
  }
  // Two-way sync: if bound to a signal, push external signal changes into the TextInput.
  if (signalName) {
    const cap = signalName.charAt(0).toUpperCase() + signalName.slice(1);
    parts.push(
      `${i(2)}Connections {`,
      `${i(3)}target: __self`,
      `${i(3)}function on${cap}Changed() { if (${inpId}.text !== ${safeName(signalName)}) ${inpId}.text = ${safeName(signalName)} }`,
      `${i(2)}}`,
    );
  }
  parts.push(`${i(1)}}`, `${pad}}`);
  return parts;
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
  const props: Props = { classes: [], classList: [], onClick: undefined, ref: undefined };
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

/** <For each={E}>{(item) => …}</For> → Repeater { model: E; <delegate with item=modelData> }. */
function emitFor(propsArg: t.Node | undefined, children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const each = readEach(propsArg, scope);
  const pad = INDENT.repeat(level);
  const lines = [`${pad}Repeater {`, ...guardLine(guard, level), `${pad}${INDENT}model: ${each}`];
  const delegate = children.find((c) => t.isArrowFunctionExpression(c) || t.isFunctionExpression(c)) as t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (delegate) {
    const param = delegate.params[0];
    const itemName = param && t.isIdentifier(param) ? param.name : null;
    const inner: Scope = { ...scope, locals: { ...(scope.locals ?? {}), ...(itemName ? { [itemName]: "modelData" } : {}) } };
    const body = delegate.body;
    if (isHCall(body)) lines.push(...emitQml(body, inner, level + 1));
    else throw new Error("For delegate must return a single element in this plan");
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
      out.push(`${pad}Repeater {`, `${pad}${INDENT}model: (${guard}) ? 1 : 0`);
      out.push(...emitQml(k, scope, level + 1));
      out.push(`${pad}}`);
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
