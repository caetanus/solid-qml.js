import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals, analyzeSetup, analyzeProps, effectDeps, analyzeResources, collectFetcherDeps, analyzeMutableLocals, analyzeHelpers } from "../src/model/symbols.ts";

async function componentFn(src: string): Promise<t.Function> {
  const { ast } = await normalize(src, "f.tsx");
  let fn: t.Function | null = null;
  const render = findRender(ast)!;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  if (!fn) throw new Error("no component fn");
  void render;
  return fn;
}

test("analyzeSignals: finds a createSignal pair (getter + setter)", async () => {
  const fn = await componentFn(`export function C(){ const [count,setCount]=createSignal(0); return <div></div>; }`);
  const table = analyzeSignals(fn);
  const getter = table.get("count");
  assert.ok(getter && getter.kind === "signal", "count is a signal");
  assert.equal(getter.kind === "signal" && getter.setter, "setCount");
  const setter = table.get("setCount");
  assert.ok(setter && setter.kind === "setter", "setCount is a setter");
  assert.equal(setter.kind === "setter" && setter.signal, "count");
});

test("analyzeSignals: captures the initial value expression", async () => {
  const fn = await componentFn(`export function C(){ const [n,setN]=createSignal(5); return <div></div>; }`);
  const sym = table_get_signal(analyzeSignals(fn), "n");
  assert.ok(sym.init && t.isNumericLiteral(sym.init, { value: 5 }));
});

test("analyzeSignals: no signals → empty table", async () => {
  const fn = await componentFn(`export function C(){ return <div></div>; }`);
  assert.equal(analyzeSignals(fn).size, 0);
});

function table_get_signal(table: Map<string, any>, name: string) {
  const s = table.get(name);
  if (!s || s.kind !== "signal") throw new Error(`${name} is not a signal`);
  return s;
}

test("analyzeSetup: separates setup statements from reactive decls and the render", async () => {
  const { ast } = await normalize(
    `import { createSignal, onCleanup } from "solid-js";
     export function C(){ const [n,setN]=createSignal(0); const t=setInterval(()=>setN(n()+1),1000); onCleanup(()=>clearInterval(t)); return <div/>; }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x:any)=>x.type==="ExportNamedDeclaration").declaration;
  const { setup, onMount } = analyzeSetup(fn);
  assert.equal(setup.length, 2);       // the setInterval decl + the onCleanup call (NOT the signal, NOT the return)
  assert.equal(onMount.length, 0);
});

test("analyzeSetup: collects onMount bodies", async () => {
  const { ast } = await normalize(
    `import { onMount } from "solid-js"; export function C(){ onMount(()=>{ globalThis.x=1; }); return <div/>; }`, "c.tsx");
  const fn = (ast as any).program.body.find((x:any)=>x.type==="ExportNamedDeclaration").declaration;
  const { setup, onMount } = analyzeSetup(fn);
  assert.equal(setup.length, 0);
  assert.equal(onMount.length, 1);
});

test("analyzeProps: mergeProps records defaults + alias; reads via alias count as used", async () => {
  const { ast } = await normalize(
    `import { mergeProps } from "solid-js"; export function G(props){ const m = mergeProps({greeting:"Hello", name:"stranger"}, props); return <text>{m.greeting} {m.name}</text>; }`, "g.tsx");
  const fn = (ast as any).program.body.find((x:any)=>x.type==="ExportNamedDeclaration").declaration;
  const r = analyzeProps(fn);
  assert.deepEqual(r.used.sort(), ["greeting", "name"]);
  assert.deepEqual(r.aliases, ["m"]);
  assert.ok(r.defaults.greeting && r.defaults.name);
});

test("analyzeSetup: collects createEffect bodies and excludes them from setup", async () => {
  const { ast } = await normalize(
    `import { createSignal, createEffect } from "solid-js";
     export function C() {
       const [a, setA] = createSignal(1);
       const [b, setB] = createSignal(0);
       createEffect(() => setB(a() * 2));
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const { setup, onMount, effects } = analyzeSetup(fn);
  assert.equal(setup.length, 0, "createEffect should not appear in setup");
  assert.equal(onMount.length, 0);
  assert.equal(effects.length, 1, "one effect body collected");
});

test("effectDeps: returns signal names read by the effect body", async () => {
  const { ast } = await normalize(
    `import { createSignal, createEffect } from "solid-js";
     export function C() {
       const [a, setA] = createSignal(1);
       const [b, setB] = createSignal(0);
       createEffect(() => setB(a() * 2));
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  assert.equal(effects.length, 1);
  const deps = effectDeps(effects[0], table);
  assert.ok(deps.includes("a"), "effect reads signal a");
  assert.ok(!deps.includes("b"), "setB is a setter, not a reactive dep");
  assert.ok(!deps.includes("setB"), "setB is a setter, not a reactive dep");
});

test("effectDeps: a member-property key named like a signal is not a false-positive dep", async () => {
  const { ast } = await normalize(
    `import { createSignal, createEffect } from "solid-js";
     export function C() {
       const [a, setA] = createSignal(1);
       const [c, setC] = createSignal(0);
       // body reads signal a() reactively, and globalThis.c as a plain member (NOT the signal c).
       createEffect(() => setC(globalThis.c + a()));
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  assert.deepEqual(deps, ["a"], "only a() is a reactive dep; globalThis.c member-key must not add c");
});

test("analyzeResources: extracts [name, source, fetcher] from createResource", async () => {
  const { ast } = await normalize(
    `import { createSignal, createResource } from "solid-js";
     const fetchUser = async (login) => (await fetch("u/" + login)).json();
     export function C() {
       const [login, setLogin] = createSignal("solidjs");
       const [user] = createResource(login, fetchUser);
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const resources = analyzeResources(fn);
  assert.equal(resources.length, 1);
  assert.equal(resources[0].name, "user");
  assert.equal(resources[0].source, "login");
  assert.equal(resources[0].fetcher, "fetchUser");
});

test("analyzeResources: registers the resource name in the signal table (kind: resource)", async () => {
  const { ast } = await normalize(
    `import { createSignal, createResource } from "solid-js";
     const f = async (l) => l;
     export function C() {
       const [login] = createSignal("x");
       const [user] = createResource(login, f);
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const sym = table.get("user");
  assert.ok(sym && sym.kind === "resource", "user is a resource symbol");
});

test("collectFetcherDeps: gathers the fetcher + transitively-referenced module-level helpers", async () => {
  const { ast } = await normalize(
    `import { createSignal, createResource } from "solid-js";
     const fetchUser = async (login) => (await fetch("u/" + login)).json();
     export function C() {
       const [login] = createSignal("x");
       const [user] = createResource(login, fetchUser);
       return <div/>;
     }`,
    "c.tsx");
  const file = ast as t.File;
  const names = collectFetcherDeps(file, "fetchUser").map((n) =>
    t.isFunctionDeclaration(n) ? n.id!.name : (n.declarations[0].id as t.Identifier).name);
  // async-to-promises injects _async/_await helpers; both must be collected so the inlined fetcher resolves.
  assert.ok(names.includes("fetchUser"), "includes the fetcher itself");
  assert.ok(names.includes("_async"), "includes the _async helper");
  assert.ok(names.includes("_await"), "includes the _await helper");
});

test("analyzeProps: splitProps first element aliases props", async () => {
  const { ast } = await normalize(
    `import { splitProps } from "solid-js"; export function T(props){ const [local] = splitProps(props, ["label"]); return <text>{local.label}</text>; }`, "t.tsx");
  const fn = (ast as any).program.body.find((x:any)=>x.type==="ExportNamedDeclaration").declaration;
  const r = analyzeProps(fn);
  assert.deepEqual(r.used, ["label"]);
  assert.deepEqual(r.aliases, ["local"]);
});

test("analyzeProps: nested member access props.todo.done registers 'todo', not 'todo.done'", async () => {
  const { ast } = await normalize(
    `export function TodoRow(props) { return <text>{props.todo.done ? "y" : "n"}</text>; }`, "f.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const r = analyzeProps(fn);
  assert.deepEqual(r.used, ["todo"], "only the top-level prop name 'todo' is registered, not 'todo.done'");
});

test("analyzeProps: function props (props.onToggle) and object props (props.todo.X) all register correctly", async () => {
  const { ast } = await normalize(
    `export function TodoRow(props) {
       return <div>
         <button onClick={() => props.onToggle(props.todo.id)}>{props.todo.done ? "y" : "n"}</button>
         <text>{props.todo.title}</text>
         <button onClick={() => props.onRemove(props.todo.id)}>X</button>
       </div>;
     }`, "f.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const r = analyzeProps(fn);
  assert.deepEqual(r.used.sort(), ["onRemove", "onToggle", "todo"], "registers todo/onToggle/onRemove, not nested keys");
});

test("analyzeMutableLocals: let x = 1 mutated by x++ is detected", async () => {
  const fn = await componentFn(`export function F() { let nextId = 1; const add = () => nextId++; return <div></div>; }`);
  const m = analyzeMutableLocals(fn);
  assert.ok(m.has("nextId"), "nextId is a mutable local");
  assert.equal(m.size, 1);
});

test("analyzeMutableLocals: let x = 1 mutated by assignment is detected", async () => {
  const fn = await componentFn(`export function F() { let n = 0; const reset = () => { n = 5; }; return <div></div>; }`);
  const m = analyzeMutableLocals(fn);
  assert.ok(m.has("n"), "n is a mutable local");
});

test("analyzeMutableLocals: let x not mutated is NOT promoted", async () => {
  const fn = await componentFn(`export function F() { let msg = "hi"; return <div>{msg}</div>; }`);
  const m = analyzeMutableLocals(fn);
  assert.equal(m.size, 0, "msg is not mutated so not promoted");
});

test("analyzeMutableLocals: const x is never promoted (immutable)", async () => {
  const fn = await componentFn(`export function F() { const x = 5; return <div>{x}</div>; }`);
  const m = analyzeMutableLocals(fn);
  assert.equal(m.size, 0, "const cannot be promoted");
});

test("analyzeMutableLocals: createSignal var is excluded (reactive, not mutable local)", async () => {
  const fn = await componentFn(`export function F() { let n = 0; const [count, setCount] = createSignal(0); setCount(n + 1); n = 2; return <div>{count()}</div>; }`);
  const m = analyzeMutableLocals(fn);
  assert.ok(m.has("n"), "n is mutable local");
  assert.ok(!m.has("count") && !m.has("setCount"), "reactive decls are excluded");
});

test("analyzeSetup: mutable locals are excluded from setup statements", async () => {
  const fn = await componentFn(`export function F() { let nextId = 1; const add = () => nextId++; return <div></div>; }`);
  const mutableLocals = new Set(analyzeMutableLocals(fn).keys());
  const { setup } = analyzeSetup(fn, mutableLocals);
  // The `let nextId = 1` declaration should NOT appear in setup (it's a property now)
  assert.ok(!setup.some((s) => s.type === "VariableDeclaration"), "let nextId excluded from setup");
});

test("analyzeHelpers: FunctionDeclaration becomes a helper", async () => {
  const fn = await componentFn(`export function C() { function inc() { setCount(count() + 1); } return <div></div>; }`);
  const helpers = analyzeHelpers(fn);
  assert.ok(helpers.has("inc"), "function declaration inc is a helper");
  const info = helpers.get("inc")!;
  assert.deepEqual(info.params, []);
  assert.equal(info.body.type, "BlockStatement");
});

test("analyzeHelpers: arrow const with params becomes a helper", async () => {
  const fn = await componentFn(`export function C() { const toggle = (id) => setTodos(id); return <div></div>; }`);
  const helpers = analyzeHelpers(fn);
  assert.ok(helpers.has("toggle"), "param-having arrow becomes a helper");
  const info = helpers.get("toggle")!;
  assert.deepEqual(info.params, ["id"]);
  assert.equal(info.body.type, "BlockStatement");
});

test("analyzeHelpers: block-bodied zero-param arrow becomes a helper", async () => {
  const fn = await componentFn(`export function C() { const selectAll = () => { doSomething(); }; return <div></div>; }`);
  const helpers = analyzeHelpers(fn);
  assert.ok(helpers.has("selectAll"), "block-bodied zero-param arrow is a helper");
});

test("analyzeHelpers: zero-param expression arrow is NOT a helper (stays derived)", async () => {
  const fn = await componentFn(`export function C() { const [count, setCount] = createSignal(0); const double = () => count() * 2; return <div></div>; }`);
  const helpers = analyzeHelpers(fn);
  assert.ok(!helpers.has("double"), "zero-param expression arrow is derived, not a helper");
});

test("analyzeHelpers: createSignal/createMemo/mergeProps consts are NOT helpers", async () => {
  const fn = await componentFn(`export function C() {
    const [count, setCount] = createSignal(0);
    const dbl = createMemo(() => count() * 2);
    const m = mergeProps({x: 1}, props);
    return <div></div>;
  }`);
  const helpers = analyzeHelpers(fn);
  assert.equal(helpers.size, 0, "reactive and prop-helper consts are not helpers");
});

test("analyzeSetup: FunctionDeclaration helpers are excluded from setup", async () => {
  const fn = await componentFn(`export function C() { function inc() { setCount(1); } return <div></div>; }`);
  const { setup } = analyzeSetup(fn);
  assert.equal(setup.length, 0, "function declaration excluded from setup");
});
