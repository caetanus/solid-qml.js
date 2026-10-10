// Step 5v-1 (virtualized CssRepeater), Task 1: transpiler certification.
// See docs/superpowers/plans/2026-10-10-step5v1-virtual-repeater.md and the "5v contract" in
// docs/superpowers/plans/2026-10-09-native-beats-web-roadmap.md.
//
// A <For> row is CERTIFIED for virtualization when its delegate is provably a passive, stateless
// element tree: the engine can then materialize only the rows in view. Certification is AST-based,
// conservative and transitive — ANY construct we have not explicitly proven safe makes the row
// NOT certified, which keeps emission byte-identical to today (eager, no `virtualize` line).
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

/** True for the row parameter itself, or a chain of plain (non-computed) member accesses rooted at
 *  it: `row`, `row.x`, `row.a.b`. */
function isRowPath(node: t.Node, itemName: string): boolean {
  if (t.isIdentifier(node, { name: itemName })) return true;
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.property)) return isRowPath(node.object, itemName);
  return false;
}

/** Safe value grammar for a certified row: the row param (bare or via member access), string and
 *  template literals whose interpolations are themselves safe, and `+` concatenations of those.
 *  Calls (including signal reads like `count()`), spreads, and any OTHER identifier (an outer-scope
 *  closure-over, a helper, a ref) are unsafe — anything not explicitly matched returns false. */
function isSafeValue(node: t.Node, itemName: string): boolean {
  if (t.isStringLiteral(node)) return true;
  if (isRowPath(node, itemName)) return true;
  if (t.isTemplateLiteral(node)) return node.expressions.every((e) => t.isExpression(e) && isSafeValue(e, itemName));
  if (t.isBinaryExpression(node) && node.operator === "+" && t.isExpression(node.left))
    return isSafeValue(node.left, itemName) && isSafeValue(node.right, itemName);
  return false;
}

/** Attributes: only `class`/`id` (safe-value grammar above) and `style` as a bare static string
 *  literal (never computed, even from the row). Any other key — classList, ref, draggable,
 *  dragData, onDrop, title, onScroll, onContextMenu, onKeyDown, any other `on*`, anything custom —
 *  or a spread (not an ObjectProperty) disqualifies the row. */
function certifyProps(propsArg: t.Node | undefined, itemName: string): boolean {
  // A normalized h() call with no attributes passes `null` (NullLiteral), not JS `undefined`.
  if (!propsArg || t.isNullLiteral(propsArg)) return true;
  if (!t.isObjectExpression(propsArg)) return false;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || p.computed || !t.isIdentifier(p.key) || !t.isExpression(p.value)) return false;
    const key = p.key.name;
    if (key === "style") { if (!t.isStringLiteral(p.value)) return false; continue; }
    if (key === "class" || key === "id") { if (!isSafeValue(p.value, itemName)) return false; continue; }
    return false; // any other attribute (including every on*) → not certified
  }
  return true;
}

/** Recursively certify one element and its subtree: a passive primitive tag, safe attributes, and
 *  children that are elements (recursing), literal text, or safe-value expressions. No spreads/
 *  refs/on* anywhere, no nested control flow, no component tags — all excluded by the checks above
 *  (tag whitelist) and the fact that a disallowed attribute key or value fails certifyProps/
 *  isSafeValue outright. */
function certifyElement(call: t.CallExpression, itemName: string): boolean {
  const { tag, props, children } = hParts(call);
  if (!t.isStringLiteral(tag) || !PASSIVE_TAGS.has(tag.value)) return false;
  if (!certifyProps(props, itemName)) return false;
  for (const child of children) {
    if (isHCall(child)) { if (!certifyElement(child, itemName)) return false; continue; }
    if (t.isStringLiteral(child) || (child as t.Node).type === "JSXText") continue;
    if (t.isExpression(child)) { if (!isSafeValue(child, itemName)) return false; continue; }
    return false;
  }
  return true;
}

/** Certify a <For> row delegate for virtualization (5v-1): one parameter (an identifier), a single
 *  h() element tree, built entirely from passive primitives per the rules above. Conservative — any
 *  uncertainty returns false, so the caller keeps today's eager emission. */
export function certifyForRow(delegate: t.ArrowFunctionExpression | t.FunctionExpression): boolean {
  if (delegate.params.length !== 1) return false;
  const param = delegate.params[0];
  if (!t.isIdentifier(param)) return false;
  const body = delegate.body;
  if (!isHCall(body)) return false;
  return certifyElement(body, param.name);
}
