import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitExpr, type Scope } from "../src/emit/expr.ts";

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
  for (const node of (ast as t.File).program.body) if (t.isVariableDeclaration(node)) found = node.declarations[0].init as t.Expression;
  return found!;
}

test("analyzeSignals: registers a createMemo as a memo", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); const dbl=createMemo(()=>count()*2); return <div></div>; }`, "binding");
  const sym = scope.table.get("dbl");
  assert.ok(sym && sym.kind === "memo");
});

test("emitExpr: a memo read is a bare property read", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); const dbl=createMemo(()=>count()*2); return <div></div>; }`, "binding");
  assert.equal(emitExpr(await expr("dbl()"), scope), "dbl");
});

test("analyzeSignals: registers a derived accessor function", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); const dbl=()=>count()*2; return <div></div>; }`, "binding");
  const sym = scope.table.get("dbl");
  assert.ok(sym && sym.kind === "derived");
});

test("emitExpr: a derived accessor call inlines its body (stays reactive in a binding)", async () => {
  const scope = await scopeFor(`export function C(){ const [count,setCount]=createSignal(0); const dbl=()=>count()*2; return <div></div>; }`, "binding");
  assert.equal(emitExpr(await expr("dbl()"), scope), "(count * 2)");
});
