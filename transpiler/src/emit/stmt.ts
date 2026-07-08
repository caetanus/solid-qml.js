import * as t from "@babel/types";
import { safeName } from "../names/safe.ts";
import { emitExpr, type Scope } from "./expr.ts";

/** Emit one statement in BINDING mode — for use inside a block-memo IIFE body where reactive cells
 *  must be read BARE (no `__self.`) so QML's binding tracker sees the dependency.
 *
 *  Supports: VariableDeclaration, ReturnStatement, ExpressionStatement.
 *  Nested functions (e.g. callbacks in `.filter(fn)`) are emitted in handler mode — they are
 *  detached closures, not binding contexts. */
export function emitBindingStmt(node: t.Statement, scope: Scope): string {
  const bScope: Scope = { ...scope, mode: "binding" };
  if (t.isVariableDeclaration(node))
    return node.declarations.map((d) =>
      `var ${safeName((d.id as t.Identifier).name)} = ${d.init ? emitExpr(d.init, bScope) : "undefined"};`).join(" ");
  if (t.isReturnStatement(node))
    return `return ${node.argument ? emitExpr(node.argument, bScope) : "undefined"};`;
  if (t.isExpressionStatement(node))
    return `${emitExpr(node.expression, bScope)};`;
  throw new Error(`emitBindingStmt: unsupported statement ${node.type}`);
}

/** Emit one component-body statement as QML-JS (imperative; runs in Component.onCompleted on __self). */
export function emitStmt(node: t.Statement, scope: Scope): string {
  const s: Scope = { ...scope, mode: "handler" };
  if (t.isVariableDeclaration(node))
    return node.declarations.map((d) =>
      `var ${safeName((d.id as t.Identifier).name)} = ${d.init ? emitValue(d.init, s) : "undefined"};`).join(" ");
  if (t.isExpressionStatement(node)) {
    const e = node.expression;
    // onCleanup(fn) → register a teardown (a root `property var __cleanups: []`), run in onDestruction.
    if (t.isCallExpression(e) && t.isIdentifier(e.callee, { name: "onCleanup" }) && e.arguments[0]) {
      return `__cleanups.push(${emitValue(e.arguments[0], s)});`;
    }
    return `${emitValue(e, s)};`;
  }
  if (t.isReturnStatement(node))
    return `return ${node.argument ? emitExpr(node.argument, s) : "undefined"};`;
  if (t.isBlockStatement(node))
    return node.body.map((st) => emitStmt(st, s)).join(" ");
  if (t.isIfStatement(node)) {
    const test = emitExpr(node.test, s);
    const consequent = emitStmt(node.consequent, s);
    if (node.alternate) {
      const alt = emitStmt(node.alternate, s);
      return `if (${test}) { ${consequent} } else { ${alt} }`;
    }
    return `if (${test}) { ${consequent} }`;
  }
  if (t.isForOfStatement(node)) {
    const left = t.isVariableDeclaration(node.left)
      ? `var ${((node.left as t.VariableDeclaration).declarations[0].id as t.Identifier).name}`
      : emitExpr(node.left as t.Expression, s);
    const right = emitExpr(node.right, s);
    const body = emitStmt(node.body, s);
    return `for (${left} of ${right}) { ${body} }`;
  }
  throw new Error(`emitStmt: unsupported statement ${node.type}`);
}

/** An expression in statement position — like emitExpr but arrows may have block bodies (lowered to
 *  a full QML-JS function). */
export function emitValue(node: t.Node, scope: Scope): string {
  if ((t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) && t.isBlockStatement(node.body)) {
    const params = node.params.map((p) => (t.isIdentifier(p) ? p.name : "_")).join(", ");
    const body = node.body.body.map((st) => emitStmt(st, scope)).join(" ");
    return `function(${params}) { ${body} }`;
  }
  return emitExpr(node, scope);
}
