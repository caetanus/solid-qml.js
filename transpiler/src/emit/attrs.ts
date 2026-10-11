// Fail-loud attribute contract (roadmap "silent drop" family): every attribute an intrinsic/builtin
// element carries is either EMITTED by that element's emitter, or REPORTED at transpile time — never
// silently ignored. This table is the contract; transpiler/test/attrs-contract.test.ts sweeps every
// tag × every attribute name the emitters know and proves the table honest (a declared attribute's
// value reaches the emitted QML; an undeclared one throws or warns).
//
// Checked once per element at emit time (emitQml), so only what is actually emitted natively is
// checked. User components and foreign .qml types are NOT checked here: an unknown property on
// them is a QML load error already (loud).
import * as t from "@babel/types";
import { hParts, isFragmentTag, isHCall } from "../ast/h.ts";

/** How an attribute's value may be written:
 *  - lit: a string literal only (a dynamic value would be ignored); `values` = the accepted set.
 *  - expr: any expression (a literal or a reactive binding).
 *  - fn: an inline function `(e) => …` (the emitter translates its body).
 *  - handler: an inline function or a context fn-member reference (emitHandler's two forms).
 *  - bool: a bare/boolean attribute or an expression (→ binding).
 *  - flag: a bare/boolean literal only.
 *  - ident: an identifier (ref={x}).
 *  - object: an object literal (classList={{ a: cond }}).
 *  - elem: an element (fallback={<text/>}, trigger={<button/>}).
 *  - key: <For key>: a field name literal (key="id") or a field accessor ((r) => r.id).
 *  - numlit: a numeric literal only (tabIndex: focus ORDER is structural, not reactive). */
type Kind = "lit" | "expr" | "fn" | "handler" | "bool" | "flag" | "ident" | "object" | "elem" | "key" | "numlit";
export type AttrSpec = Kind | { kind: "lit"; values: readonly string[] };
type Attrs = Record<string, AttrSpec>;

// The native widget modules read `class` as a literal; the elements emitted through qml.ts's
// readProps (DYN) also take a dynamic class string (split like the DOM's className).
const CSS: Attrs = { class: "lit", classList: "object" };
const DYN: Attrs = { class: "expr", classList: "object" };
// <div> and every other plain block tag (section, nav, li, a, …) → W.Div; the text tags → W.Text.
// Both share the pointer emission (emitInteractions); only a box scrolls.
//
// Keyboard & accessibility (release-plan M5): role/tabIndex/aria-* map to the SAME native semantics
// a Templates control gets for free (QtQuick's `Accessible` attached type + the tab-focus chain) —
// see docs/keyboard-accessibility.md for the full table. `role` is an open string (unknown roles
// WARN at emit time — attrs.ts only proves it is a literal, qml.ts's ARIA_ROLE_MAP decides whether
// it maps); aria-description/aria-describedby are static text (no live AT announcement on change);
// the rest are reactive. aria-expanded has NO Qt Quick equivalent (QQuickAccessibleAttached does not
// expose `expanded`/`expandable`) and stays unmapped — isWarnOnlyAttr below.
const POINTER: Attrs = {
  ref: "ident", onClick: "handler", onKeyDown: "fn", onContextMenu: "fn",
  draggable: "flag", dragData: "expr", onDrop: "fn", title: "expr",
  tabIndex: "numlit", role: "lit",
  "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  "aria-checked": "expr", "aria-pressed": "expr", "aria-selected": "expr",
  "aria-hidden": "expr", "aria-disabled": "expr",
};
const TEXT_INPUT_TYPES = ["text", "password", "email", "search", "tel", "url"] as const;
const INPUT_TYPES = [...TEXT_INPUT_TYPES, "checkbox", "radio", "range", "number", "date"] as const;

export const ELEMENT_ATTRS: Record<string, Attrs> = {
  // ── HTML ──────────────────────────────────────────────────────────────────────────────────────
  box: { ...DYN, ...POINTER, onScroll: "fn" },
  text: { ...DYN, ...POINTER },
  // aria-label/aria-description/aria-describedby (release-plan M5 follow-up): on every native
  // control that takes focus, routed by the emitter onto the inner QtQuick.Templates control's
  // Accessible.name/.description — see docs/keyboard-accessibility.md "aria-label on native
  // controls". `role` stays WARN-ONLY on these (a Templates control already carries its own role).
  button: { ...DYN, onClick: "handler", type: { kind: "lit", values: ["submit", "button"] }, "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit" },
  img: { ...DYN, src: "expr", alt: "expr" },
  "input:text": {
    ...DYN, ref: "ident", type: { kind: "lit", values: INPUT_TYPES }, value: "expr", placeholder: "expr",
    onInput: "fn", onChange: "fn", onKeyDown: "fn", onKeydown: "fn", disabled: "bool", readOnly: "bool",
    readonly: "bool", maxLength: "expr", maxlength: "expr",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  "input:checkbox": {
    ...DYN, ref: "ident", type: { kind: "lit", values: INPUT_TYPES }, checked: "expr", onChange: "fn",
    disabled: "bool", role: { kind: "lit", values: ["switch"] },
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  "input:radio": {
    ...DYN, ref: "ident", type: { kind: "lit", values: INPUT_TYPES }, checked: "expr", onChange: "fn",
    disabled: "bool", name: "lit",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  "input:range": {
    ...DYN, ref: "ident", type: { kind: "lit", values: INPUT_TYPES }, value: "expr", onInput: "fn",
    onChange: "fn", min: "expr", max: "expr", step: "expr", disabled: "bool",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  "input:number": {
    ...DYN, ref: "ident", type: { kind: "lit", values: INPUT_TYPES }, value: "expr", onChange: "fn",
    min: "expr", max: "expr", step: "expr", disabled: "bool",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  // min/max: declared so the emitter's own, more specific error ("validate in onChange") is the one shown.
  "input:date": {
    ...DYN, type: { kind: "lit", values: INPUT_TYPES }, value: "expr", onChange: "fn", disabled: "bool", min: "expr", max: "expr",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  textarea: {
    ...DYN, ref: "ident", value: "expr", placeholder: "expr", onInput: "fn", onChange: "fn",
    disabled: "bool", readOnly: "bool", readonly: "bool",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  select: {
    ...DYN, value: "expr", onChange: "fn", disabled: "bool", options: "expr",
    "aria-label": "expr", "aria-description": "lit", "aria-describedby": "lit",
  },
  option: { value: "lit" },
  progress: { ...CSS, value: "expr", max: "expr" },
  fieldset: CSS,
  legend: CSS,
  dialog: { ...CSS, open: "expr", title: "expr", onClose: "fn" },
  details: { ...CSS, open: "expr" },
  summary: CSS,
  // ── native widgets (emit/native/*) ────────────────────────────────────────────────────────────
  ToolBar: CSS,
  Swipe: { ...CSS, right: "expr", farRight: "expr", left: "expr", farLeft: "expr", onSwipe: "fn" },
  TabBar: { ...CSS, current: "expr", onChange: "fn" },
  TabButton: {},
  SplitView: { ...CSS, orientation: { kind: "lit", values: ["horizontal", "vertical"] } },
  Drawer: { ...CSS, open: "expr", onClose: "fn", size: "expr", edge: { kind: "lit", values: ["left", "right", "top", "bottom"] } },
  StackView: { ...CSS, current: "expr" },
  SwipeView: { ...CSS, current: "expr", onChange: "fn" },
  PageIndicator: { ...CSS, count: "expr", current: "expr" },
  RangeSlider: { ...CSS, first: "expr", second: "expr", min: "expr", max: "expr", step: "expr", onChange: "fn", disabled: "bool" },
  Dial: { ...CSS, value: "expr", min: "expr", max: "expr", step: "expr", onChange: "fn", disabled: "bool" },
  Tumbler: { ...CSS, options: "expr", value: "expr", onChange: "fn", disabled: "bool" },
  DelayButton: { ...CSS, delay: "expr", onActivated: "fn", disabled: "bool" },
  BusyIndicator: { ...CSS, running: "expr" },
  RoundButton: { ...CSS, onClick: "fn", disabled: "bool" },
  ToolButton: { ...CSS, onClick: "fn", disabled: "bool" },
  ToolSeparator: CSS,
  // <Menu open={…}> is retired (a Wayland popup must open synchronously): not in the contract.
  Menu: { class: "lit", ref: "ident", trigger: "elem", onClose: "fn" },
  // A <Menu> inside <MenuBar> is an OS-native Platform.Menu: only its title is read.
  "MenuBar>Menu": { title: "expr" },
  MenuItem: { onClick: "fn", disabled: "bool", checked: "expr" },
  MenuSeparator: {},
  MenuBar: {},
  // suppressWhen (solidterm mouse reporting): (modifiers) => boolean, re-checked on every
  // right-PRESS — true declines the press instead of opening, letting it fall through below.
  ContextMenu: { class: "lit", onClose: "fn", suppressWhen: "fn" },
  TreeView: { ...CSS, data: "expr", onSelect: "fn" },
  ListView: { ...CSS, data: "expr", onSelect: "fn" },
  TableView: { ...CSS, data: "expr", columns: "expr", onSelect: "fn" },
  Tray: { icon: "expr", tooltip: "expr", onActivate: "fn" },
  Chart: { class: "lit", accent: "expr", xMax: "expr" },
  Scene3D: { class: "lit", modelColor: "expr", spinning: "expr" },
  Surface: { class: "lit", heightMap: "expr", valueMin: "expr", valueMax: "expr" },
  MediaPlayer: { class: "lit", src: "expr", subtitles: "expr", autoplay: "expr" },
  WebView: { class: "lit", src: "expr", html: "expr", remoteContent: "expr" },
  RichText: { ...DYN, html: "expr", variant: "expr", onChange: "fn" },
  DocumentEditor: {
    ...DYN, ref: "ident", toolbar: "bool", onModifiedChanged: "fn", onContentChanged: "fn", onStylesChanged: "fn",
    pageWidth: "expr", pageMargins: "expr", zoom: "expr", onZoomRequested: "fn", onHeadingsChanged: "fn",
    onBlocksLoaded: "fn", onBlockChanged: "fn", onBlockRemoved: "fn", onMarkClicked: "fn", onMarkContextMenu: "fn", onMarkHovered: "fn",
  },
  CodeEditor: { class: "lit", language: "expr", text: "expr" },
  Shortcut: { keys: "expr", enabled: "expr", onActivated: "fn" },
  Calendar: { ...DYN, value: "expr", onChange: "fn" },
  // ── builtins / control flow ───────────────────────────────────────────────────────────────────
  Window: { title: "expr", width: "expr", height: "expr", visible: "flag" },
  Show: { when: "expr", fallback: "elem" },
  // key (C23): opt-in reuse by a field — see forKeyField.
  For: { each: "expr", key: "key" },
  Index: { each: "expr" },
  VirtualList: { ...DYN, ref: "ident", each: "expr", onEndReached: "fn", onKeyDown: "fn" },
  Switch: { fallback: "elem" },
  Match: { when: "expr" },
  Dynamic: { component: "expr" },
  Suspense: { fallback: "elem" },
};

/** WARN tier (documented allow-list): web attributes that have NO native effect yet and whose
 *  absence does not change what the app does. They are reported once per tag/attribute with a
 *  console warning instead of failing the build:
 *  - `data-*`: web-only data attributes (test hooks, the web runtime's markers); nothing to map.
 *  - `role`/`tabIndex`/most `aria-*` ARE mapped on a plain element (<div>/<text>, POINTER above —
 *    see docs/keyboard-accessibility.md) and on `<input role="switch">`; this tier only still
 *    catches them where the element does NOT consume them (every OTHER native widget — a Templates
 *    control already carries its own role/focus chain, so e.g. `<button role="x">` warns here
 *    instead of fighting the control's real Accessible role) and `aria-expanded`, which has no Qt
 *    Quick equivalent (QQuickAccessibleAttached exposes no `expanded`/`expandable` property) and so
 *    stays unmapped everywhere, a real gap, tracked, not a silent success.
 *  Everything else not in ELEMENT_ATTRS is a transpile error. */
export function isWarnOnlyAttr(key: string): boolean {
  return key.startsWith("data-") || key.startsWith("aria-") || key === "role" || key === "tabIndex" || key === "tabindex";
}

/** The ELEMENT_ATTRS key for an h() call, or null when the tag is not checked here (a user or
 *  foreign component, a context Provider). `textTags`: the tags emitQml maps to W.Text. */
export function attrTagOf(call: t.CallExpression, textTags: ReadonlySet<string>): string | null {
  const { tag, props } = hParts(call);
  if (isFragmentTag(tag)) return null;
  if (t.isIdentifier(tag)) {
    // A registered/builtin name shadows a user component (same precedence as emitQml's dispatch).
    return tag.name in ELEMENT_ATTRS ? tag.name : null;
  }
  if (!t.isStringLiteral(tag)) return null;
  const name = tag.value;
  if (name === "input") {
    const type = stringProp(props, "type") ?? "text";
    if ((TEXT_INPUT_TYPES as readonly string[]).includes(type)) return "input:text";
    return `input:${type}`;
  }
  if (textTags.has(name)) return "text";
  if (name in ELEMENT_ATTRS) return name;
  return "box";
}

function stringProp(props: t.Node | undefined, name: string): string | null {
  if (!props || !t.isObjectExpression(props)) return null;
  for (const p of props.properties)
    if (t.isObjectProperty(p) && propKey(p) === name && t.isStringLiteral(p.value)) return p.value.value;
  return null;
}

function propKey(p: t.ObjectProperty): string | null {
  return t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? p.key.value : null;
}

function valueOk(spec: AttrSpec, v: t.Node): string | null {
  const kind = typeof spec === "string" ? spec : spec.kind;
  const isLit = t.isStringLiteral(v) || (t.isTemplateLiteral(v) && v.expressions.length === 0);
  switch (kind) {
    case "lit": {
      if (!isLit) return "must be a string literal (a dynamic value is not supported natively)";
      const s = t.isStringLiteral(v) ? v.value : (v as t.TemplateLiteral).quasis[0].value.cooked ?? "";
      if (typeof spec !== "string" && !spec.values.includes(s))
        return `must be ${spec.values.join("|")} (got ${JSON.stringify(s)})`;
      return null;
    }
    case "expr": case "handler": case "bool":
      return t.isExpression(v) ? null : "must be an expression";
    case "fn":
      return t.isArrowFunctionExpression(v) || t.isFunctionExpression(v) ? null : "must be an inline function: (e) => …";
    case "flag":
      return t.isBooleanLiteral(v) ? null : "must be a bare/boolean attribute (a dynamic value is not supported natively)";
    case "ident":
      return t.isIdentifier(v) ? null : "must be a variable: ref={x}";
    case "object":
      return t.isObjectExpression(v) ? null : "must be an object literal: classList={{ name: cond }}";
    case "elem":
      return isHCall(v) ? null : "must be an element — wrap text in <text>…</text>";
    case "key":
      return forKeyFieldOf(v) !== null ? null
        : "must be a field name (key=\"id\") or a field accessor (key={(r) => r.id}); a computed key " +
          "function is not supported natively — key the rows by a field of the row object";
    case "numlit": {
      const n = t.isUnaryExpression(v) && v.operator === "-" ? v.argument : v;
      return t.isNumericLiteral(n) ? null : "must be a numeric literal (focus order is structural, not reactive)";
    }
  }
}

/** <For key> (C23): the row field that keys the rows — from `key="id"` or `key={(r) => r.id}` /
 *  `key={(r) => r["id"]}`; null when the value is neither (a computed key: not supported). */
export function forKeyFieldOf(v: t.Node): string | null {
  const field = (s: string) => /^[A-Za-z_$][\w$]*$/.test(s) ? s : null;
  if (t.isStringLiteral(v)) return field(v.value);
  if (t.isTemplateLiteral(v) && v.expressions.length === 0) return field(v.quasis[0].value.cooked ?? "");
  if ((t.isArrowFunctionExpression(v) || t.isFunctionExpression(v)) && v.params.length === 1 && t.isIdentifier(v.params[0])) {
    let body: t.Node = v.body;
    if (t.isBlockStatement(body)) {
      if (body.body.length !== 1 || !t.isReturnStatement(body.body[0]) || !body.body[0].argument) return null;
      body = body.body[0].argument;
    }
    const p = (v.params[0] as t.Identifier).name;
    if (!t.isMemberExpression(body) || !t.isIdentifier(body.object, { name: p })) return null;
    if (!body.computed && t.isIdentifier(body.property)) return field(body.property.name);
    if (body.computed && t.isStringLiteral(body.property)) return field(body.property.value);
  }
  return null;
}

/** The <For>'s key field (C23), or null without a `key` attribute (reference identity, the default). */
export function forKeyField(propsArg: t.Node | undefined): string | null {
  if (!propsArg || !t.isObjectExpression(propsArg)) return null;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || propKey(p) !== "key") continue;
    const f = forKeyFieldOf(p.value);
    if (f === null) throw new Error(`<For> key ${valueOk("key", p.value)}`);
    return f;
  }
  return null;
}

/** A <For> whose rows are not CssRepeater rows (<select> options, menu items) has no row reuse to opt
 *  into: `key` there is rejected instead of silently ignored. */
export function rejectForKey(propsArg: t.Node | undefined, where: string): void {
  if (!propsArg || !t.isObjectExpression(propsArg)) return;
  for (const p of propsArg.properties)
    if (t.isObjectProperty(p) && propKey(p) === "key")
      throw new Error(`${where}<For key> is not supported: these rows are rebuilt from the list on every change ` +
        "(no row reuse to opt into) — drop `key`");
}

/** Attributes that USED to exist, rejected with a migration hint instead of the generic error. */
const RETIRED: Record<string, string> = {
  "Menu open": "<Menu open={…}> is retired: a Wayland popup window can't be opened from a deferred reactive " +
    "signal (the input grab fails). Use <Menu ref={r}> and call r.open(x, y) SYNCHRONOUSLY inside " +
    "the click/handler that should open it.",
};

/** An attribute that is only read together with another one. */
export const REQUIRES: Record<string, string> = { dragData: "draggable" };

const label = (tag: string) => tag === "box" ? "div" : tag.startsWith("input:") ? `input type=${tag.slice(6)}` : tag.replace(">", "> <");
const warned = new Set<string>();
/** Forget which warnings were printed (one per tag/attribute per generate run). */
export function resetAttrWarnings(): void { warned.clear(); }

/** Print `msg` once per `key` per generate run (the same de-dupe set checkAttrs's own web-only
 *  warnings use, reset together by resetAttrWarnings). Used by qml.ts for the a11y warnings that
 *  depend on an attribute's VALUE, not just its name (an unknown `role="…"`, a positive tabIndex). */
export function warnOnce(key: string, msg: string): void {
  if (warned.has(key)) return;
  warned.add(key);
  console.warn(msg);
}

/** Check one element's attributes against the contract: throw on anything its emitter would drop,
 *  warn (once) on the documented web-only allow-list. `tagKey` from attrTagOf. */
export function checkAttrs(tagKey: string, propsArg: t.Node | undefined): void {
  const spec = ELEMENT_ATTRS[tagKey];
  if (!spec && tagKey.startsWith("input:"))
    throw new Error(`<input type="${tagKey.slice(6)}"> is not supported natively (supported types: ${INPUT_TYPES.join(", ")})`);
  if (!spec) throw new Error(`internal: no attribute contract for <${label(tagKey)}>`);
  if (!propsArg || t.isNullLiteral(propsArg)) return;
  if (!t.isObjectExpression(propsArg))
    throw new Error(`<${label(tagKey)}>: spread attributes ({...props}) are not supported natively — pass each attribute explicitly`);
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p)) throw new Error(`<${label(tagKey)}>: spread attributes ({...props}) are not supported natively`);
    const key = propKey(p);
    if (key === null || key === "children") continue;
    const s = spec[key];
    if (s === undefined) {
      const retired = RETIRED[`${tagKey} ${key}`];
      if (retired) throw new Error(retired);
      if (isWarnOnlyAttr(key)) {
        const k = `${tagKey}:${key}`;
        if (!warned.has(k)) {
          warned.add(k);
          console.warn(`[solid-qml] <${label(tagKey)} ${key}=…>: web-only attribute, no native effect (ignored)`);
        }
        continue;
      }
      const ok = Object.keys(spec);
      throw new Error(`<${label(tagKey)}> does not support \`${key}\` natively (it would be silently dropped)` +
        (ok.length ? `; supported: ${ok.join(", ")}` : "; it takes no attributes"));
    }
    const bad = valueOk(s, p.value);
    if (bad) throw new Error(`<${label(tagKey)}> ${key} ${bad}`);
  }
  const keys = propsArg.properties.filter(t.isObjectProperty).map(propKey);
  // <Menu trigger> is self-managed: a ref belongs to the imperative form and would be ignored.
  if (tagKey === "Menu" && keys.includes("trigger") && keys.includes("ref"))
    throw new Error("<Menu trigger={…}> manages its own open state: drop ref (or drop trigger for the imperative r.open(x, y) form)");
  for (const [k, needs] of Object.entries(REQUIRES))
    if (k in spec && keys.includes(k) && !keys.includes(needs))
      throw new Error(`<${label(tagKey)}> ${k} has no effect without ${needs}`);
}

/** Children consumed by their PARENT's emitter (never dispatched through emitQml themselves): their
 *  attributes are checked when the parent is. */
export const PARENT_CONSUMED = new Set(["option", "summary", "TabButton", "MenuItem", "MenuSeparator", "Menu", "Match"]);

const checked = new WeakSet<t.Node>();
/** Check an element (once) and, recursively, the parent-consumed children it carries. */
export function checkElement(call: t.CallExpression, textTags: ReadonlySet<string>, parent?: string): void {
  if (checked.has(call)) return;
  checked.add(call);
  const own = attrTagOf(call, textTags);
  // A parent-specific contract (`MenuBar>Menu`) wins over the tag's own.
  const key = own && parent && `${parent}>${own}` in ELEMENT_ATTRS ? `${parent}>${own}` : own;
  const { props, children } = hParts(call);
  if (key) checkAttrs(key, props);
  for (const k of children) {
    if (!isHCall(k)) continue;
    const { tag } = hParts(k as t.CallExpression);
    const name = t.isIdentifier(tag) ? tag.name : t.isStringLiteral(tag) ? tag.value : null;
    if (name && PARENT_CONSUMED.has(name)) checkElement(k as t.CallExpression, textTags, own ?? undefined);
  }
}
