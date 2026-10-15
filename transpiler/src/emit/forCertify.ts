// Step 5v-1 (virtualized CssRepeater), Task 1: transpiler certification.
// See docs/superpowers/plans/2026-10-10-step5v1-virtual-repeater.md and the "5v contract" in
// docs/superpowers/plans/2026-10-09-native-beats-web-roadmap.md.
//
// A <For> row is CERTIFIED for virtualization when its delegate is provably a passive, stateless
// element tree: the engine can then materialize only the rows in view. Certification is AST-based,
// conservative and transitive — ANY construct we have not explicitly proven safe makes the row
// NOT certified, which keeps emission byte-identical to today (eager, no `virtualize` line).
//
// A virtualized row is created when it enters the window, destroyed or parked in a bounded pool when it
// leaves, and REBOUND (new modelData/index) when it comes back: its bindings are re-evaluated any number
// of times, so they must have no side effect, and the row's SIZE must stay predictable (the list places
// rows it has not materialized from measurements and estimates — see certifyStyle). Every rejection
// carries a reason (forRowCertification), so a test — or a reader — can see why a row stays eager.
import * as t from "@babel/types";
import { hParts, isHCall } from "../ast/h.ts";
import { kebabCssProperty } from "./inlineStyle.ts";

/** Passive primitives eligible for certification: the lowercase tags that reach W.Text (mirrors
 *  TEXT_TAGS in qml.ts) plus the generic block primitive W.Div ("div" itself; other block tags
 *  like "section"/"article" are NOT included here — add them deliberately, with a reason, not by
 *  default). Everything else a row could use — button/img/input/textarea/select, "a" (link),
 *  "label", any tag the native-tag registry owns (progress/dialog/ToolBar/Menu/…), any control-flow
 *  keyword (Show/For/Index/Switch/…) or user/builtin component — is excluded BY CONSTRUCTION below:
 *  their h() tag is never a matching string literal (control-flow/component/native-widget tags are
 *  Identifiers or MemberExpressions, never a plain lowercase string). */
const PASSIVE_TAGS = new Set(["div", "text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

/** What the certifier knows about the component the <For> lives in. */
export interface CertifyContext {
  /** True when `name()` (no arguments) reads a component signal or memo: emitted as a plain QML
   *  property read (expr.ts cellRef), so it is pure and re-evaluable, and the binding follows it live. */
  isReactiveRead?: (name: string) => boolean;
  /** True when `name(…)` is a signal or store setter (a write: a side effect). */
  isSetter?: (name: string) => boolean;
}

export type Certification = { ok: true } | { ok: false; reason: string };

/** Thrown inside the walk to carry the first reason out; never escapes this module. */
class Reject extends Error {}
function reject(reason: string): never {
  throw new Reject(reason);
}

interface Row {
  item: string;
  /** The <For> delegate's second parameter (Solid's index ACCESSOR, `i()`), when it has one. */
  index: string | null;
  ctx: CertifyContext;
}

/** True for the row parameter itself, or a chain of plain (non-computed) member accesses rooted at
 *  it: `row`, `row.x`, `row.a.b`. */
function isRowPath(node: t.Node, itemName: string): boolean {
  if (t.isIdentifier(node, { name: itemName })) return true;
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.property)) return isRowPath(node.object, itemName);
  return false;
}

function describe(node: t.Node): string {
  if (t.isIdentifier(node)) return `\`${node.name}\``;
  if (t.isCallExpression(node) && t.isIdentifier(node.callee)) return `a call to \`${node.callee.name}\``;
  if (t.isCallExpression(node)) return "a call";
  return `a ${node.type}`;
}

/** Safe value grammar for `class`/`id` and text children: the row param (bare or via member access),
 *  string and template literals whose interpolations are themselves safe, and `+` concatenations of
 *  those. Calls (including signal reads like `count()`), spreads, and any OTHER identifier (an
 *  outer-scope closure-over, a helper, a ref) are unsafe — anything not explicitly matched rejects. */
function safeValue(node: t.Node, row: Row, where: string): void {
  if (t.isStringLiteral(node)) return;
  if (isRowPath(node, row.item)) return;
  if (t.isTemplateLiteral(node)) {
    for (const e of node.expressions) safeValue(e, row, where);
    return;
  }
  if (t.isBinaryExpression(node) && node.operator === "+" && t.isExpression(node.left)) {
    safeValue(node.left, row, where);
    safeValue(node.right, row, where);
    return;
  }
  reject(`${where}: ${describe(node)} (only the row item, literals, template literals and + of those)`);
}

const PURE_BINARY = new Set(["+", "-", "*", "/", "%", "**", "<", "<=", ">", ">=", "==", "!=", "===", "!=="]);
const PURE_UNARY = new Set(["-", "+", "!"]);

/** Owner rule (2026-10-15): "a virtualização é segura sempre que o size é previsível" — a row may be
 *  virtualized whenever its SIZE is predictable. So a `style` key is classified by whether it can change
 *  the row's geometry, not by where its value comes from.
 *
 *  PAINT-ONLY keys (kebab-case, as emitted): the engine reads them only to paint — never in its layout
 *  pass (CssLayoutStyle::parse in vendor/qml-css-engine/src/qmlcss/csslength.cpp reads display/position/
 *  flex/gap/sizes/margin/padding/border WIDTHS/aspect-ratio/order; text measurement reads the font keys).
 *  Verified one by one in the engine: backgrounds are the render shell / fill layers (cssrect, cssfill);
 *  `color` is the text colour; `opacity` and `visibility: hidden` stay in flow (cssrect "STILL in flow");
 *  `transform` is the item's own transform (cssrect parseTransform), outside the flow; border colour,
 *  style and radius and the outline are the shell's paint (the border WIDTH is layout: `border`,
 *  `border-width` and their sides are NOT here); box-shadow and text-shadow are shadow items;
 *  text-decoration is the font's underline/strike. The engine has no list of its own to share, so this
 *  one is closed and conservative: a key not in it — including custom properties (`--x`, a var() can
 *  feed a layout property) — is treated as layout-affecting. */
const PAINT_ONLY = new Set([
  "background", "background-color", "background-image", "background-position", "background-size", "background-repeat",
  "color", "opacity", "visibility", "transform",
  "border-color", "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
  "border-style", "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
  "border-radius", "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
  "border-bottom-left-radius",
  "box-shadow", "text-shadow",
  "outline", "outline-color", "outline-width", "outline-style", "outline-offset",
  "text-decoration", "text-decoration-line", "text-decoration-color", "text-decoration-style",
]);

export function isPaintOnlyStyleProperty(kebab: string): boolean {
  return PAINT_ONLY.has(kebab);
}

/** A paint-only value may be any expression: whatever it evaluates to, the row keeps its size. It must
 *  still have no side effect — a virtualized row re-evaluates it on every rebind, an eager one does not —
 *  so an assignment, `++`/`--`, `delete` or a signal/store setter call anywhere in it rejects. */
function paintValue(node: t.Expression, row: Row, where: string): void {
  let bad: string | null = null;
  t.traverseFast(node, (n) => {
    if (bad) return;
    if (t.isAssignmentExpression(n) || t.isUpdateExpression(n) || (t.isUnaryExpression(n) && n.operator === "delete"))
      bad = `a side effect (${n.type})`;
    else if (t.isCallExpression(n) && t.isIdentifier(n.callee) && row.ctx.isSetter?.(n.callee.name))
      bad = `a side effect (the setter \`${n.callee.name}\`)`;
  });
  if (bad) reject(`${where}: ${bad}`);
}

/** A layout-affecting value keeps the size predictable when it depends on nothing but the row's own
 *  record: literals, the row item and its member paths, and operators over those. The virtualized list
 *  measures every logical row it materializes and keeps that measurement until the row's record, the
 *  width, the font or the rule set changes (CssRepeater::dropMeasurement / checkMeasureKey) — the same
 *  guarantee that covers class={row.status}. Anything else — the row index (rows keep their measurement
 *  when they move), a component signal or memo (it would resize rows outside the window without a
 *  re-measure), a call, an outer identifier — would let a measurement go stale: the row stays eager. */
function rowValue(node: t.Node, row: Row, where: string): void {
  if (t.isStringLiteral(node) || t.isNumericLiteral(node) || t.isBooleanLiteral(node) || t.isNullLiteral(node)) return;
  if (isRowPath(node, row.item)) return;
  if (t.isTemplateLiteral(node)) {
    for (const e of node.expressions) rowValue(e, row, where);
    return;
  }
  if (t.isUnaryExpression(node) && PURE_UNARY.has(node.operator)) return rowValue(node.argument, row, where);
  if ((t.isBinaryExpression(node) && PURE_BINARY.has(node.operator) && t.isExpression(node.left)) || t.isLogicalExpression(node)) {
    rowValue(node.left, row, where);
    rowValue(node.right, row, where);
    return;
  }
  if (t.isConditionalExpression(node)) {
    rowValue(node.test, row, where);
    rowValue(node.consequent, row, where);
    rowValue(node.alternate, row, where);
    return;
  }
  let what = describe(node);
  if (t.isCallExpression(node) && t.isIdentifier(node.callee) && node.arguments.length === 0) {
    if (node.callee.name === row.index) what = `the row index \`${row.index}()\``;
    else if (node.callee.name !== row.item && row.ctx.isReactiveRead?.(node.callee.name))
      what = `the component signal/memo \`${node.callee.name}()\``;
  }
  reject(`${where}: layout property depends on ${what} — the row's size would not be predictable`);
}

/** A `style` attribute value. A static string: fixed. An object literal with static keys: each key is
 *  classified (paint-only → any side-effect-free value; layout → a value from the row's own record).
 *  Anything else — a spread or computed key, a template string with interpolations, an object or string
 *  computed at runtime — has properties unknown at compile time: it cannot be classified, so eager. */
function certifyStyle(v: t.Node, row: Row): void {
  if (t.isStringLiteral(v)) return;
  if (t.isTemplateLiteral(v) && v.expressions.length === 0) return;
  if (!t.isObjectExpression(v))
    reject("style: not an object literal — its properties are not known at compile time");
  for (const p of v.properties) {
    if (t.isSpreadElement(p)) reject("style: a spread in the style object — its properties are not known at compile time");
    if (!t.isObjectProperty(p)) reject(`style: a ${p.type} in the style object`);
    if (p.computed || !(t.isIdentifier(p.key) || t.isStringLiteral(p.key)))
      reject("style: a computed key in the style object — its properties are not known at compile time");
    const prop = kebabCssProperty(t.isIdentifier(p.key) ? p.key.name : (p.key as t.StringLiteral).value);
    if (!t.isExpression(p.value)) reject(`style.${prop}: a ${p.value.type}`);
    if (PAINT_ONLY.has(prop)) paintValue(p.value, row, `style.${prop}`);
    else rowValue(p.value, row, `style.${prop}`);
  }
}

/** Attributes: `class`/`id` (safe-value grammar) and `style` (classified per key, certifyStyle). Any other key —
 *  classList, ref, draggable, dragData, onDrop, title, onScroll, onContextMenu, onKeyDown, any other
 *  `on*`, anything custom — or a spread (not an ObjectProperty) disqualifies the row. */
function certifyProps(propsArg: t.Node | undefined, row: Row): void {
  // A normalized h() call with no attributes passes `null` (NullLiteral), not JS `undefined`.
  if (!propsArg || t.isNullLiteral(propsArg)) return;
  if (!t.isObjectExpression(propsArg)) reject("attributes are not an object literal");
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || p.computed || !t.isIdentifier(p.key) || !t.isExpression(p.value))
      reject("a spread or computed attribute");
    const key = p.key.name;
    if (key === "style") { certifyStyle(p.value, row); continue; }
    if (key === "class" || key === "id") { safeValue(p.value, row, key); continue; }
    reject(`attribute \`${key}\``); // any other attribute (including every on*) → not certified
  }
}

/** Recursively certify one element and its subtree: a passive primitive tag, safe attributes, and
 *  children that are elements (recursing), literal text, or safe-value expressions. No spreads/
 *  refs/on* anywhere, no nested control flow, no component tags — all excluded by the checks above
 *  (tag whitelist) and the fact that a disallowed attribute key or value rejects outright. */
function certifyElement(call: t.CallExpression, row: Row): void {
  const { tag, props, children } = hParts(call);
  if (!t.isStringLiteral(tag)) reject(`${describe(tag)} is not a passive primitive tag`);
  if (!PASSIVE_TAGS.has(tag.value)) reject(`<${tag.value}> is not a passive primitive tag`);
  certifyProps(props, row);
  for (const child of children) {
    if (isHCall(child)) { certifyElement(child, row); continue; }
    if (t.isStringLiteral(child) || (child as t.Node).type === "JSXText") continue;
    if (t.isExpression(child)) { safeValue(child, row, "child"); continue; }
    reject(`child: a ${(child as t.Node).type}`);
  }
}

/** Certify a <For> row delegate for virtualization: the row item (an identifier) and optionally the
 *  index accessor (an identifier), a single h() element tree, built entirely from passive primitives
 *  per the rules above. Conservative — any uncertainty is a rejection with its reason, and the caller
 *  keeps today's eager emission. */
export function forRowCertification(delegate: t.ArrowFunctionExpression | t.FunctionExpression,
                                    ctx: CertifyContext = {}): Certification {
  const [param, indexParam] = delegate.params;
  if (delegate.params.length < 1 || delegate.params.length > 2) return { ok: false, reason: "the row function takes 1 or 2 parameters" };
  if (!t.isIdentifier(param)) return { ok: false, reason: "the row parameter is not an identifier" };
  if (indexParam && !t.isIdentifier(indexParam)) return { ok: false, reason: "the index parameter is not an identifier" };
  const body = delegate.body;
  if (!isHCall(body)) return { ok: false, reason: "the row function does not return a single element" };
  try {
    certifyElement(body, { item: param.name, index: indexParam ? (indexParam as t.Identifier).name : null, ctx });
    return { ok: true };
  } catch (e) {
    if (e instanceof Reject) return { ok: false, reason: e.message };
    throw e;
  }
}

export function certifyForRow(delegate: t.ArrowFunctionExpression | t.FunctionExpression, ctx: CertifyContext = {}): boolean {
  return forRowCertification(delegate, ctx).ok;
}
