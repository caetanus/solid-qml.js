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
  assert.ok(deps.some((d) => d.target === "__self" && d.prop === "a"), "effect reads signal a");
  assert.ok(!deps.some((d) => d.prop === "b"), "setB is a setter, not a reactive dep");
  assert.ok(!deps.some((d) => d.prop === "setB"), "setB is a setter, not a reactive dep");
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
  assert.deepEqual(deps, [{ target: "__self", prop: "a" }], "only a() is a reactive dep; globalThis.c is a builtin global, not a context property");
});

test("effectDeps: a context/global QObject member read inside an if (office.spellReady)", async () => {
  // The real office-app bug: createEffect(() => { if (office.spellReady) recheckAll(); }) where
  // `office` is a C++ context property (QObject, NOTIFY spellReadyChanged) — not a Solid signal,
  // not a prop, not in any table. Must still be tracked.
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => { if (office.spellReady) recheckAll(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  assert.deepEqual(deps, [{ target: "office", prop: "spellReady" }]);
});

test("effectDeps: second real case — office.picked read as a JSON.parse argument", async () => {
  // createEffect(() => { if (office.picked) onPicked(JSON.parse(office.picked)); }) — JSON is a
  // builtin global (excluded); office.picked (read twice) dedupes to one dep.
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => { if (office.picked) onPicked(JSON.parse(office.picked)); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  assert.deepEqual(deps, [{ target: "office", prop: "picked" }]);
});

test("effectDeps: a method CALL on a context object is not a dependency (action, not a read)", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => { office.recheckAll(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  assert.deepEqual(deps, []);
});

test("effectDeps: props.x read routes to the self property x", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C(props) {
       createEffect(() => { if (props.x) doSomething(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table, { propsParam: "props" });
  assert.deepEqual(deps, [{ target: "__self", prop: "x" }]);
});

test("effectDeps: a store field read tracks the whole store object (reassign-to-rebind)", async () => {
  const { ast } = await normalize(
    `import { createEffect, createStore } from "solid-js/store";
     export function C() {
       const [store, setStore] = createStore({ field: 1 });
       createEffect(() => { if (store.field) doSomething(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  assert.deepEqual(deps, [{ target: "__self", prop: "store" }]);
});

test("effectDeps: a nested member chain a.b.c tracks every prefix level", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => { if (cfg.user.theme) applyTheme(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table);
  const sorted = [...deps].sort((a, b) => a.target.length - b.target.length);
  assert.deepEqual(sorted, [
    { target: "cfg", prop: "user" },
    { target: "cfg.user", prop: "theme" },
  ]);
});

test("effectDeps: a read inside a called local helper is followed (closure over component state)", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       function helperCheck() { return office.spellReady; }
       createEffect(() => { if (helperCheck()) recheckAll(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const helpersMap = analyzeHelpers(fn);
  const deps = effectDeps(effects[0], table, { helpers: helpersMap });
  assert.deepEqual(deps, [{ target: "office", prop: "spellReady" }]);
});

test("effectDeps: a called helper's OWN param is shadowed, not mistaken for a global (office app regression)", async () => {
  // Real office-app shape (WriterApp.tsx): onPicked's own param `p` must NOT be tracked as if `p`
  // were a top-level context property, even though `p.seq`/`p.kind`/`p.path` look exactly like the
  // office.spellReady/picked shape syntactically. `lastPick` (a signal, read inside the helper) IS
  // still tracked — Solid's dependency tracking follows the synchronous call, not just the body
  // the effect lexically wrote.
  const { ast } = await normalize(
    `import { createSignal, createEffect } from "solid-js";
     export function C() {
       const [lastPick, setLastPick] = createSignal(0);
       const onPicked = (p) => {
         if (p.seq <= lastPick()) return;
         setLastPick(p.seq);
         doThing(p.path);
       };
       createEffect(() => { if (office.picked) onPicked(JSON.parse(office.picked)); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const helpersMap = analyzeHelpers(fn);
  const deps = effectDeps(effects[0], table, { helpers: helpersMap });
  const sorted = [...deps].sort((a, b) => (a.target + a.prop).localeCompare(b.target + b.prop));
  assert.deepEqual(sorted, [
    { target: "__self", prop: "lastPick" },
    { target: "office", prop: "picked" },
  ]);
  assert.ok(!deps.some((d) => d.target === "p"), "onPicked's own param p must never be a dep target");
});

test("effectDeps: a local declared INSIDE the effect body itself is shadowed too (solid-mail regression)", async () => {
  // Real solid-mail shape #1: `var n = JSON.parse(mail.noticeOpen); … n.category … n.id` — `n` is
  // a plain local declared directly in the effect body (not inside a called helper); it must not
  // be mistaken for a top-level identifier named `n`.
  const { ast: ast0 } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => {
         if (mail.noticeOpen === "") return;
         var n = JSON.parse(mail.noticeOpen);
         doThing(n.category, n.id);
       });
       return <div/>;
     }`,
    "c.tsx");
  const fn0 = (ast0 as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table0 = analyzeSignals(fn0);
  const deps0 = effectDeps(analyzeSetup(fn0).effects[0], table0);
  assert.deepEqual(deps0, [{ target: "mail", prop: "noticeOpen" }]);

  // Real solid-mail shape #2: `accounts.map(function(a) { return a.id })` where `accounts` is a
  // signal — `accounts` IS tracked (bare identifier, independent of the method call), but the
  // callback's own param `a` must NOT be (it shadows any outer `a`).
  const { ast: ast1 } = await normalize(
    `import { createSignal, createEffect } from "solid-js";
     export function C() {
       const [accounts, setAccounts] = createSignal([]);
       createEffect(() => {
         var ids = accounts().map(function (a) { return a.id; });
         doOther(ids);
       });
       return <div/>;
     }`,
    "c.tsx");
  const fn1 = (ast1 as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table1 = analyzeSignals(fn1);
  const deps1 = effectDeps(analyzeSetup(fn1).effects[0], table1);
  assert.deepEqual(deps1, [{ target: "__self", prop: "accounts" }]);
});

test("effectDeps: without the helpers map, a read inside a called local helper is NOT analyzable (documented gap)", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       function helperCheck() { return office.spellReady; }
       createEffect(() => { if (helperCheck()) recheckAll(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table); // no `helpers` passed
  assert.deepEqual(deps, []);
});

test("effectDeps: a useContext accessor member is tracked on its provider instance; a fn member is not", async () => {
  const { ast } = await normalize(
    `import { createEffect } from "solid-js";
     export function C() {
       createEffect(() => { if (c.count) c.increment(); });
       return <div/>;
     }`,
    "c.tsx");
  const fn = (ast as any).program.body.find((x: any) => x.type === "ExportNamedDeclaration").declaration;
  const table = analyzeSignals(fn);
  const { effects } = analyzeSetup(fn);
  const deps = effectDeps(effects[0], table, {
    ctxBindings: { c: "Counter" },
    ctxValueShape: { Counter: { count: "accessor", increment: "fn" } },
  });
  assert.deepEqual(deps, [{ target: "__ctx_Counter", prop: "count" }]);
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
