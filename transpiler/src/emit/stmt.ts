import * as t from "@babel/types";
import { safeName } from "../names/safe.ts";
import { emitExpr, emitPattern, shadowOwnParams, type Scope } from "./expr.ts";

/** Emit one statement in BINDING mode — for use inside a block-memo IIFE body where reactive cells
 *  must be read BARE (no `__self.`) so QML's binding tracker sees the dependency. Same statement set
 *  as `emitStmt`; nested functions (callbacks in `.filter(fn)`) are detached closures. */
export function emitBindingStmt(node: t.Statement, scope: Scope): string {
  return emitStatement(node, { ...scope, mode: "binding" }, false);
}

/** Emit one component-body statement as QML-JS (imperative; runs in Component.onCompleted on __self). */
export function emitStmt(node: t.Statement, scope: Scope): string {
  return emitStatement(node, { ...scope, mode: "handler" }, false);
}

/** A statement list (a function body, a block): names it declares SHADOW any same-named entry
 *  inherited in `scope.locals` (a module-const alias `__const_X`, a ref alias, an event-handler
 *  substitution…) for the whole list — real JS lexical scoping, so a local `const ROWS = …` inside a
 *  helper is not rewritten to the module's `__const_ROWS`. */
export function emitStatements(list: t.Statement[], scope: Scope, nested = false): string {
  const inner = shadowDeclared(list, scope);
  return list.map((st) => emitStatement(st, inner, nested)).join(" ");
}

function shadowDeclared(list: t.Statement[], scope: Scope): Scope {
  if (!scope.locals) return scope;
  let locals: Record<string, string> | undefined;
  const shadow = (name: string) => {
    if (!(name in scope.locals!)) return;
    locals = locals ?? { ...scope.locals };
    locals[name] = safeName(name);
  };
  for (const st of list) {
    if (t.isVariableDeclaration(st))
      for (const d of st.declarations) for (const n of Object.keys(t.getBindingIdentifiers(d.id))) shadow(n);
    else if (t.isFunctionDeclaration(st) && st.id) shadow(st.id.name);
  }
  return locals ? { ...scope, locals } : scope;
}

/** `let`/`const` keep their block scoping when NESTED (inside a block, a loop, a branch): emitted as
 *  `var` there, a nested `const x` would overwrite an outer `x`, and a closure created per loop
 *  iteration would see only the last value. At a function body's top level the declaration is
 *  emitted as `var` (the historical form): several setup/onMount bodies are concatenated into one
 *  Component.onCompleted block, where two `const x` would be a redeclaration error. */
function declKind(node: t.VariableDeclaration, nested: boolean): string {
  return nested && node.kind !== "var" ? node.kind : "var";
}

function emitDeclarators(node: t.VariableDeclaration, s: Scope, kind: string): string {
  return `${kind} ${node.declarations.map((d) =>
    `${emitPattern(d.id, s)} = ${d.init ? emitValue(d.init, s) : "undefined"}`).join(", ")}`;
}

/** The loop head's declaration (`for (let i = 0; …)`, `for (const x of …)`): always keeps its kind —
 *  a `let`/`const` head gives each iteration its own binding (closures created in the body see
 *  their own iteration's value, as in JS). */
function loopHead(left: t.VariableDeclaration | t.LVal | t.Expression | null | undefined, s: Scope): string {
  if (!left) return "";
  if (t.isVariableDeclaration(left)) {
    return left.declarations.map((d, i) =>
      `${i === 0 ? `${left.kind} ` : ""}${emitPattern(d.id, s)}${d.init ? ` = ${emitExpr(d.init, s)}` : ""}`).join(", ");
  }
  return emitExpr(left as t.Expression, s);
}

function emitStatement(node: t.Statement, s: Scope, nested: boolean): string {
  if (t.isVariableDeclaration(node)) return `${emitDeclarators(node, s, declKind(node, nested))};`;
  if (t.isExpressionStatement(node)) return `${emitValue(node.expression, s)};`;
  if (t.isReturnStatement(node))
    return `return ${node.argument ? emitValue(node.argument, s) : "undefined"};`;
  if (t.isBlockStatement(node)) return nested ? `{ ${emitStatements(node.body, s, true)} }` : emitStatements(node.body, s, true);
  if (t.isEmptyStatement(node)) return ";";
  if (t.isIfStatement(node)) {
    const test = emitExpr(node.test, s);
    const consequent = emitStatement(node.consequent, s, true);
    if (node.alternate) return `if (${test}) { ${unblock(consequent)} } else { ${unblock(emitStatement(node.alternate, s, true))} }`;
    return `if (${test}) { ${unblock(consequent)} }`;
  }
  if (t.isForOfStatement(node))
    return `for (${loopHead(node.left, s)} of ${emitExpr(node.right, s)}) { ${unblock(emitStatement(node.body, s, true))} }`;
  if (t.isForInStatement(node))
    return `for (${loopHead(node.left, s)} in ${emitExpr(node.right, s)}) { ${unblock(emitStatement(node.body, s, true))} }`;
  if (t.isForStatement(node)) {
    const init = node.init ? (t.isVariableDeclaration(node.init) ? loopHead(node.init, s) : emitExpr(node.init, s)) : "";
    const test = node.test ? emitExpr(node.test, s) : "";
    const update = node.update ? emitExpr(node.update, s) : "";
    return `for (${init}; ${test}; ${update}) { ${unblock(emitStatement(node.body, s, true))} }`;
  }
  if (t.isWhileStatement(node))
    return `while (${emitExpr(node.test, s)}) { ${unblock(emitStatement(node.body, s, true))} }`;
  if (t.isDoWhileStatement(node))
    return `do { ${unblock(emitStatement(node.body, s, true))} } while (${emitExpr(node.test, s)});`;
  if (t.isSwitchStatement(node)) {
    // Every case shares the switch's block scope (as in JS): shadow the names declared by any case.
    const inner = shadowDeclared(node.cases.flatMap((c) => c.consequent), s);
    const cases = node.cases.map((c) => {
      const head = c.test ? `case ${emitExpr(c.test, inner)}:` : "default:";
      return `${head} ${c.consequent.map((st) => emitStatement(st, inner, true)).join(" ")}`;
    });
    return `switch (${emitExpr(node.discriminant, s)}) { ${cases.join(" ")} }`;
  }
  if (t.isTryStatement(node)) {
    let out = `try { ${unblock(emitStatement(node.block, s, true))} }`;
    if (node.handler) {
      const p = node.handler.param;
      const inner = p && t.isIdentifier(p) ? shadowOwnParams(s, [p]) : s;
      out += ` catch${p ? ` (${emitPattern(p, s)})` : ""} { ${unblock(emitStatement(node.handler.body, inner, true))} }`;
    }
    if (node.finalizer) out += ` finally { ${unblock(emitStatement(node.finalizer, s, true))} }`;
    return out;
  }
  if (t.isThrowStatement(node)) return `throw ${emitExpr(node.argument, s)};`;
  if (t.isBreakStatement(node)) return node.label ? `break ${node.label.name};` : "break;";
  if (t.isContinueStatement(node)) return node.label ? `continue ${node.label.name};` : "continue;";
  if (t.isLabeledStatement(node)) return `${node.label.name}: ${emitStatement(node.body, s, true)}`;
  if (t.isFunctionDeclaration(node) && node.id) {
    // A nested `function f(){}` inside a function body (hoisted, like JS).
    const params = node.params.map((p) => emitPattern(p, s)).join(", ");
    const inner = shadowOwnParams(s, node.params);
    return `function ${safeName(node.id.name)}(${params}) { ${emitStatements(node.body.body, inner)} }`;
  }
  throw new Error(`unsupported statement in component code: ${node.type} — ${STATEMENT_HINT[node.type] ?? "rewrite it with the supported statements (if/else, for, for…of, for…in, while, do…while, switch, try/catch/finally, throw, break/continue)"}`);
}

const STATEMENT_HINT: Record<string, string> = {
  ClassDeclaration: "declare the class in a plain .ts module and import it (loaded as a JS module, unmodified)",
  WithStatement: "`with` is not allowed",
};

/** A statement emitted in a nested position that is itself a block already carries its braces;
 *  the callers above wrap bodies in `{ … }`, so strip one level to keep the output flat. */
function unblock(code: string): string {
  return code.startsWith("{ ") && code.endsWith(" }") ? code.slice(2, -2) : code;
}

/** An expression in statement position — like emitExpr but arrows may have block bodies (lowered to
 *  a full QML-JS function). */
export function emitValue(node: t.Node, scope: Scope): string {
  if ((t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) && t.isBlockStatement(node.body)) {
    const params = node.params.map((p) => (t.isIdentifier(p) ? p.name : emitPattern(p, scope))).join(", ");
    const inner = shadowOwnParams(scope, node.params, (n) => n); // signature above uses the bare name, not safeName
    return `function(${params}) { ${emitStatements(node.body.body, inner)} }`;
  }
  return emitExpr(node, scope);
}
