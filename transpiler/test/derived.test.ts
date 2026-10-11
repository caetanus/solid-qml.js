import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals, analyzeHelpers } from "../src/model/symbols.ts";
import { emitExpr, type Scope } from "../src/emit/expr.ts";
import { emitComponentType } from "../src/emit/component.ts";

async function componentFn(src: string): Promise<t.Function> {
  const { ast } = await normalize(src, "f.tsx");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  return fn!;
}
async function scopeFor(src: string, mode: Scope["mode"]): Promise<Scope> {
  const fn = await componentFn(src);
  return { table: analyzeSignals(fn), mode };
}
async function expr(code: string): Promise<t.Expression> {
  const { ast } = await normalize(`const __x = (${code});`, "e.tsx");
  let found: t.Expression | null = null;
  for (const node of (ast as t.File).program.body) if (t.isVariableDeclaration(node)) found = node.declarations[0].init as t.Expression;
  return found!;
}
/** Full component emission (render tree + properties + methods + onCompleted), for asserting on
 *  whether a promoted derived accessor is emitted as a real QML method AND whether its use sites
 *  (binding call / non-call reference) route to it correctly — emitExpr alone can't show either,
 *  since the method decl is only added by emitComponentType's helpers loop. */
async function qmlType(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const render = findRender(ast);
  if (!render) throw new Error("no render");
  const fn = await componentFn(src);
  return emitComponentType(fn, render, new Map()).join("\n");
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

// --- Bug: a call-only derived is inlined and correctly never emitted as a function. A derived with
// ANY non-call reference (passed by value) has nowhere to inline INTO, and today is never emitted at
// all — the reference site just emits the bare, undeclared name, which is a QML
// "ReferenceError: f is not defined" at RUNTIME (no transpile-time error). The rule: call-only stays
// inlined (byte-identical to today); any non-call reference promotes it to a real QML method (the
// same mechanism analyzeHelpers already uses for param/block-bodied arrows), and every reference
// (called or not) routes to that method instead of the inlined expression.

test("baseline: a call-only derived is NOT promoted to a helper (today's inlining is untouched)", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; return <div>{f()}</div>; }`);
  const table = analyzeSignals(fn);
  assert.ok(table.get("f")?.kind === "derived", "f stays a derived accessor (inlined)");
  assert.ok(!analyzeHelpers(fn).has("f"), "f is not emitted as a method when every use is a direct call");
});

test("register(f): a derived passed by reference is promoted to a real method", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; register(f); return <div></div>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined, "f is no longer classified as an inlined derived");
  const helper = analyzeHelpers(fn).get("f");
  assert.ok(helper, "f is emitted as a helper (real QML method)");
  assert.deepEqual(helper!.params, []);
});

test("onClick={f}: a derived referenced (not called) as a JSX prop is promoted", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; return <button onClick={f}>Go</button>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined);
  assert.ok(analyzeHelpers(fn).has("f"));
});

test("setTimeout(f, 0): a derived passed to a callback-taking call is promoted", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; setTimeout(f, 0); return <div></div>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined);
  assert.ok(analyzeHelpers(fn).has("f"));
});

test("arr.map(f): a derived passed as a bare callback argument is promoted", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; const arr=[1,2]; arr.map(f); return <div></div>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined);
  assert.ok(analyzeHelpers(fn).has("f"));
});

test("{cb: f}: a derived stored in an object literal is promoted", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; const handlers={cb:f}; register(handlers); return <div></div>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined);
  assert.ok(analyzeHelpers(fn).has("f"));
});

test("returned f: a derived returned by value from a sibling helper is promoted", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; function getF(){ return f; } register(getF); return <div></div>; }`);
  const table = analyzeSignals(fn);
  assert.equal(table.get("f"), undefined);
  assert.ok(analyzeHelpers(fn).has("f"));
});

test("shadowing: a nested param/local named f is not a reference to the derived — f stays inlined", async () => {
  const fn = await componentFn(
    `export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; ` +
    `const arr=[1,2,3]; arr.map(function(f){ return f.id; }); return <div>{f()}</div>; }`,
  );
  const table = analyzeSignals(fn);
  assert.ok(table.get("f")?.kind === "derived", "the outer f is untouched by the shadowing local param");
  assert.ok(!analyzeHelpers(fn).has("f"));
});

test("mixed call+reference: f() stays a method call (not inlined) once f is promoted", async () => {
  const src = `export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; register(f); return <div>{f()}</div>; }`;
  const out = await qmlType(src);
  // The method is actually emitted…
  assert.match(out, /function f\(\) \{ return count \* 2; \}/);
  // …and the binding call site routes to it (a plain call), NOT the old inlined expression form.
  assert.match(out, /"" \+ \(f\(\)\)/);
  assert.doesNotMatch(out, /"" \+ \(\(count \* 2\)\)/); // no leftover inlined body in the binding
});

test("register(f): emitted QML declares the method and passes it bare (no ReferenceError shape)", async () => {
  const src = `export function C(){ const [count,setCount]=createSignal(0); const f=()=>count()*2; register(f); return <div></div>; }`;
  const out = await qmlType(src);
  assert.match(out, /function f\(\) \{ return count \* 2; \}/);
  assert.match(out, /register\(f\);/);
});
