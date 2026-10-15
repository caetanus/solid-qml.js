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
// leaves, and REBOUND (new modelData/index) when it comes back: every binding in it must be a pure
// function of its inputs, re-evaluable any number of times, with no side effect. Every rejection carries
// a reason (forRowCertification), so a test — or a reader — can see why a row stays eager.
import * as t from "@babel/types";
import { hParts, isHCall } from "../ast/h.ts";

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

/** Pure value grammar for `style` values. Beyond the safe grammar above: number/boolean/null literals,
 *  the row index accessor `i()`, zero-argument reads of a component signal or memo (`tone()`), and
 *  arithmetic, comparison, logical and conditional operators over those. Each of these emits a QML
 *  expression made of literals, `modelData`/`__forIndex` and component property reads: no side effect,
 *  re-evaluable on every rebind, and live (the binding re-runs when the signal changes, in the visible
 *  rows and in the parked ones alike). Any other call, identifier, `new`, assignment, `in`/`instanceof`,
 *  a function or a computed member access rejects. */
function pureValue(node: t.Node, row: Row, where: string): void {
  if (t.isStringLiteral(node) || t.isNumericLiteral(node) || t.isBooleanLiteral(node) || t.isNullLiteral(node)) return;
  if (isRowPath(node, row.item)) return;
  if (t.isTemplateLiteral(node)) {
    for (const e of node.expressions) pureValue(e, row, where);
    return;
  }
  if (t.isUnaryExpression(node) && PURE_UNARY.has(node.operator)) return pureValue(node.argument, row, where);
  if (t.isBinaryExpression(node) && PURE_BINARY.has(node.operator) && t.isExpression(node.left)) {
    pureValue(node.left, row, where);
    pureValue(node.right, row, where);
    return;
  }
  if (t.isLogicalExpression(node)) {
    pureValue(node.left, row, where);
    pureValue(node.right, row, where);
    return;
  }
  if (t.isConditionalExpression(node)) {
    pureValue(node.test, row, where);
    pureValue(node.consequent, row, where);
    pureValue(node.alternate, row, where);
    return;
  }
  if (t.isCallExpression(node) && t.isIdentifier(node.callee) && node.arguments.length === 0) {
    const name = node.callee.name;
    if (name === row.index) return;
    if (name !== row.item && row.ctx.isReactiveRead?.(name)) return;
  }
  reject(`${where}: ${describe(node)} (only the row item, the row index i(), literals, component signals/memos, and operators over those)`);
}

/** A `style` attribute value: a static string, a template literal or object literal built from pure
 *  values, or a bare pure value (an object or string computed at runtime from the row — the engine's
 *  map setter converts it). An object literal must have static keys and no spread. */
function certifyStyle(v: t.Node, row: Row): void {
  if (t.isStringLiteral(v)) return;
  if (t.isObjectExpression(v)) {
    for (const p of v.properties) {
      if (t.isSpreadElement(p)) reject("style: a spread in the style object");
      if (!t.isObjectProperty(p)) reject(`style: a ${p.type} in the style object`);
      if (p.computed || !(t.isIdentifier(p.key) || t.isStringLiteral(p.key))) reject("style: a computed key in the style object");
      const key = t.isIdentifier(p.key) ? p.key.name : (p.key as t.StringLiteral).value;
      pureValue(p.value, row, `style.${key}`);
    }
    return;
  }
  pureValue(v, row, "style");
}

/** Attributes: `class`/`id` (safe-value grammar) and `style` (pure-value grammar). Any other key —
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
