import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

// Release-plan M2 ("no silent miscompiles"), item 4: the statements and expressions V4 runs natively
// (while/do…while, classic for, switch, try/catch/finally, throw, labels, `new`, RegExp literals,
// optional chaining, destructuring, object spread lowered to Object.assign) are emitted as JS with
// only the names rewritten — they used to be rejected as "unsupported". Each positive case is also
// EXECUTED: the emitted method is lifted out of the QML and run in Node (the same JS V4 runs), so a
// test proves the semantics, not just the spelling.

const HEAD = `import { createSignal } from "solid-js";\n`;
async function qmlOf(body: string): Promise<string> {
  const app = await generate(HEAD + body, "/virtual/stmts.tsx");
  return [app.entry, ...Object.values(app.components)].join("\n");
}
/** Lift `function <name>(…) { … }` (one emitted line) out of the QML and evaluate it in Node. */
function method(qml: string, name: string): (...a: unknown[]) => unknown {
  const line = qml.split("\n").find((l) => l.trim().startsWith(`function ${name}(`));
  assert.ok(line, `method ${name} not emitted:\n${qml}`);
  return new Function(`return (${line!.trim()})`)() as (...a: unknown[]) => unknown;
}
const app = (helpers: string) => `${helpers}
export function App() { const [s, setS] = createSignal(0); return <button onClick={() => setS(run(s()))}>{s()}</button>; }`;

test("statements: classic for, while, do…while, labelled break/continue run as in JS", async () => {
  const qml = await qmlOf(app(`function run(n) {
    let t = 0;
    for (let i = 0; i < n; i++) t += i;
    let j = 0; while (j < 3) j++;
    do { j++; } while (j < 5);
    outer: for (const a of [1, 2, 3]) { for (const b of [1, 2]) { if (b === 2) continue outer; if (a === 3) break outer; t += 100; } }
    return t + j;
  }`));
  assert.equal(method(qml, "run")(4), 0 + 1 + 2 + 3 + 5 + 200);
});

test("statements: switch with fallthrough/default and block-scoped cases", async () => {
  const qml = await qmlOf(app(`function run(n) {
    let out = "";
    switch (n) { case 0: out += "zero"; case 1: { const x = "one"; out += x; break; } default: out = "many"; }
    return out;
  }`));
  const f = method(qml, "run");
  assert.equal(f(0), "zeroone");
  assert.equal(f(1), "one");
  assert.equal(f(7), "many");
});

test("statements: try/catch/finally and throw", async () => {
  const qml = await qmlOf(app(`function run(src) {
    let log = [];
    try { log.push(JSON.parse(src)); if (src === "2") throw new Error("two"); }
    catch (e) { log.push(e instanceof SyntaxError ? "syntax" : e.message); }
    finally { log.push("fin"); }
    try { log.push("x"); } catch { log.push("never"); }
    return log.join(",");
  }`));
  const f = method(qml, "run");
  assert.equal(f("1"), "1,fin,x");
  assert.equal(f("{"), "syntax,fin,x");
  assert.equal(f("2"), "2,two,fin,x");
});

test("expressions: new, RegExp literals, optional chaining, sequence", async () => {
  const qml = await qmlOf(app(`function run(o) {
    const m = new Map([["a", 1]]);
    const re = /b+/g;
    return [m.get("a"), "abbbcb".replace(re, "_"), o?.x?.y, o?.["x"]?.y ?? "none", o?.f?.(2), (1, 2)].join("|");
  }`));
  const f = method(qml, "run");
  assert.equal(f({ x: { y: 7 }, f: (n: number) => n * 3 }), "1|a_c_|7|7|6|2");
  assert.equal(f(null), "1|a_c_||none||2");
});

test("expressions: object spread is lowered to Object.assign (V4 has none); computed keys and methods", async () => {
  const qml = await qmlOf(app(`function run(n) {
    const base = { a: 1, b: 2 };
    const k = "c";
    const o = { ...base, b: n, [k]: 3, twice() { return this.b * 2; }, ...{ d: 4 } };
    return JSON.stringify(o) + o.twice();
  }`));
  assert.match(qml, /Object\.assign\(\{\}, base, \(\{ b: n, \[k\]: 3, twice: function\(\) \{ return this\.b \* 2; \} \}\), \(\{ d: 4 \}\)\)/);
  assert.equal(method(qml, "run")(5), `{"a":1,"b":5,"c":3,"d":4}10`);
});

test("destructuring: declarations, params, for…of heads, defaults and rest (array)", async () => {
  const qml = await qmlOf(app(`function run({ a, b: [c, d = 4] }, ...more) {
    const [x, , y = 9, ...rest] = [1, 2, undefined, 5, 6];
    let total = 0;
    for (const [k, v] of Object.entries({ p: 10, q: 20 })) total += v + k.length;
    const pick = ({ n }) => n * 2;
    return [a, c, d, x, y, rest.length, more.length, total, pick({ n: 3 })].join(",");
  }`));
  assert.equal(method(qml, "run")({ a: 1, b: [3] }, 7, 8), "1,3,4,1,9,2,2,32,6");
});

test("let/const keep block scope when nested: per-iteration closures and shadowing", async () => {
  const qml = await qmlOf(app(`function run(n) {
    const fns = [];
    for (let i = 0; i < 3; i++) fns.push(() => i);
    for (const v of [7, 8]) fns.push(() => v);
    const x = 1;
    if (n) { const x = 2; fns.push(() => x); }
    return fns.map((f) => f()).join(",") + ":" + x;
  }`));
  assert.equal(method(qml, "run")(1), "0,1,2,7,8,2:1");
});

test("a local declaration shadows a module-const alias of the same name", async () => {
  const qml = await qmlOf(`const ROWS = [1, 2, 3];
function run(n) { const ROWS = [n]; return ROWS.length; }
export function App() { const [s, setS] = createSignal(0); return <button onClick={() => setS(run(s()))}>{ROWS.length}</button>; }`);
  assert.equal(method(qml, "run")(5), 1);
});

test("nested function declarations and for…in", async () => {
  const qml = await qmlOf(app(`function run(o) {
    function keys(x) { const out = []; for (const k in x) out.push(k); return out; }
    return keys(o).join("");
  }`));
  assert.equal(method(qml, "run")({ a: 1, b: 2 }), "ab");
});

test("statements also work in component setup code, handlers and block memos", async () => {
  const qml = await qmlOf(`import { createMemo } from "solid-js";
export function App() {
  const [s, setS] = createSignal(3);
  const total = createMemo(() => { let t = 0; for (let i = 0; i < s(); i++) t += i; return t; });
  function go() { let i = 0; while (i < 2) i++; switch (i) { case 2: setS(s() + 1); break; } }
  return <button onClick={go}>{total()}</button>;
}`);
  assert.match(qml, /readonly property var total: \(function\(\) \{ var t = 0; for \(let i = 0; i < s; i\+\+\) \{ t \+= i; \} return t; \}\)\(\)/);
  assert.match(qml, /function go\(\) \{ var i = 0; while \(i < 2\) \{ i\+\+; \} switch \(i\) \{ case 2: s = s \+ 1; break; \} \}/);
});

// ── still unsupported: a clear compile error naming the construct and the way out ──────────────
const rejects = async (body: string, msg: RegExp) => {
  await assert.rejects(qmlOf(app(body)), msg);
};
test("fail loud: object rest in a pattern (V4 has none)", () =>
  rejects(`function run(o) { const { a, ...rest } = o; return rest; }`, /object rest in a destructuring pattern .* not supported by the V4 engine/));
test("fail loud: BigInt literal", () =>
  rejects(`function run(o) { return String(10n); }`, /unsupported expression in component code: BigIntLiteral — the V4 engine has no BigInt/));
test("fail loud: class declaration in component code", () =>
  rejects(`function run(o) { class K {} return new K(); }`, /unsupported statement in component code: ClassDeclaration — declare the class in a plain \.ts module/));
test("fail loud: tagged template", () =>
  rejects(`function run(o) { return String.raw\`a\${o}\`; }`, /TaggedTemplateExpression — call the tag function explicitly/));
