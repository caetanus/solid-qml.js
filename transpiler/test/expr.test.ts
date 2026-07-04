import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitExpr, type Scope } from "../src/emit/expr.ts";
import { emitBindingStmt, emitStmt } from "../src/emit/stmt.ts";

async function scopeFor(src: string, mode: Scope["mode"]): Promise<Scope> {
  const { ast } = await normalize(src, "f.tsx");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  return { table: analyzeSignals(fn!), mode };
}

async function expr(code: string): Promise<t.Expression> {
  const { ast } = await normalize(`const __x = (${code});`, "e.tsx");
  let found: t.Expression | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isVariableDeclaration(node)) found = node.declarations[0].init as t.Expression;
  }
  return found!;
}

test("emitExpr: a signal getter call reads the bare property", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "binding");
  assert.equal(emitExpr(await expr("count()"), scope), "count");
});

test("emitExpr: arithmetic around a signal read", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "binding");
  assert.equal(emitExpr(await expr("count() + 1"), scope), "count + 1");
});

test("emitExpr: a setter value call becomes a property assignment", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("setCount(count() + 1)"), scope), "count = count + 1");
});

test("emitExpr: a setter updater arrow becomes an assignment with the prev bound to the property", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("setCount(c => c + 1)"), scope), "count = count + 1");
});

test("emitExpr: literals and a non-signal identifier pass through", async () => {
  const scope = await scopeFor(`export function C(){ return <div></div>; }`, "binding");
  assert.equal(emitExpr(await expr("42"), scope), "42");
  assert.equal(emitExpr(await expr(`"hi"`), scope), '"hi"');
  assert.equal(emitExpr(await expr("foo"), scope), "foo");
});

test("emitExpr: an expression-bodied arrow becomes a QML-JS function", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("() => setCount(count() + 1)"), scope), "function() { return count = count + 1 }");
});

test("emitExpr: arrow params are preserved", async () => {
  const scope = await scopeFor(`export function C(){ return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("(a, b) => a + b"), scope), "function(a, b) { return a + b }");
});

test("emitExpr: a block-bodied callback in an expression emits in handler mode", async () => {
  // `.filter(function(t){ return t > 0; })` — block body callback inside a call expr
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "binding");
  const result = emitExpr(await expr("items.filter(function(t){ return t > 0; })"), scope);
  assert.equal(result, "items.filter(function(t) { return t > 0; })");
});

test("emitBindingStmt: VariableDeclaration with a signal read emits bare (no __self.)", async () => {
  const scope = await scopeFor(`export function C(){ const [filter,setFilter]=createSignal("all"); return <div></div>; }`, "binding");
  // Parse: const f = filter();
  const { ast } = await normalize(`function __wrap(){ const f = filter(); }`, "s.tsx");
  const fn = (ast as t.File).program.body[0] as t.FunctionDeclaration;
  const stmt = fn.body.body[0];
  assert.equal(emitBindingStmt(stmt, scope), "var f = filter;");
});

test("emitBindingStmt: ReturnStatement emits bare signal read", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "binding");
  const { ast } = await normalize(`function __wrap(){ return count() * 2; }`, "s.tsx");
  const fn = (ast as t.File).program.body[0] as t.FunctionDeclaration;
  const stmt = fn.body.body[0];
  assert.equal(emitBindingStmt(stmt, scope), "return count * 2;");
});

test("emitExpr: UnaryExpression (!) emits correctly", async () => {
  const scope = await scopeFor(`export function C(){ const [done,setDone]=createSignal(false); return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("!done()"), scope), "!done");
});

test("emitExpr: UnaryExpression (-) negates expression", async () => {
  const scope = await scopeFor(`export function C(){ return <div></div>; }`, "handler");
  assert.equal(emitExpr(await expr("-num"), scope), "-num");
});

async function stmtIn(code: string): Promise<t.Statement> {
  const { ast } = await normalize(`function __wrap(){ ${code} }`, "s.tsx");
  const fn = (ast as t.File).program.body[0] as t.FunctionDeclaration;
  return fn.body.body[0];
}

test("emitStmt: ReturnStatement emits return with expression", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  const stmt = await stmtIn("return count() + 1;");
  assert.equal(emitStmt(stmt, scope), "return count + 1;");
});

test("emitStmt: ReturnStatement with no argument emits 'return undefined'", async () => {
  const scope = await scopeFor(`export function C(){ return <div></div>; }`, "handler");
  const stmt = await stmtIn("return;");
  assert.equal(emitStmt(stmt, scope), "return undefined;");
});

test("emitStmt: IfStatement without else emits correctly", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  const stmt = await stmtIn("if (!count()) { setCount(1); }");
  assert.equal(emitStmt(stmt, scope), "if (!count) { count = 1; }");
});

test("emitStmt: IfStatement with else emits both branches", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  const stmt = await stmtIn("if (count() > 0) { setCount(0); } else { setCount(1); }");
  assert.equal(emitStmt(stmt, scope), "if (count > 0) { count = 0; } else { count = 1; }");
});

test("emitStmt: ForOfStatement emits bare signal read in iterable", async () => {
  const scope = await scopeFor(`export function C(){ const [items,setItems]=createSignal([]); return <div></div>; }`, "handler");
  const stmt = await stmtIn("for (const todo of items()) { doSomething(todo); }");
  const result = emitStmt(stmt, scope);
  assert.match(result, /for \(var todo of items\)/);
  assert.match(result, /doSomething\(todo\)/);
});

test("emitStmt: BlockStatement emits all child statements", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`, "handler");
  const stmt = await stmtIn("{ setCount(1); setCount(2); }");
  assert.equal(emitStmt(stmt, scope), "count = 1; count = 2;");
});

test("expr: word unary operators keep their separating space (typeof/void/delete)", async () => {
  const { normalize } = await import("../src/babel/transform.ts");
  const { emitExpr } = await import("../src/emit/expr.ts");
  const t = (await import("@babel/types"));
  const { ast } = await normalize(`const x = typeof process !== "undefined";`, "f.ts");
  let expr;
  const body = (ast as any).program.body;
  expr = body[0].declarations[0].init;
  const out = emitExpr(expr, { table: new Map(), mode: "binding" } as any);
  if (!/typeof process/.test(out)) throw new Error("missing space: " + out);
});
