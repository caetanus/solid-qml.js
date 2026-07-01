import { test } from "node:test";
import assert from "node:assert/strict";
import { normalize } from "../src/babel/transform.ts";

test("normalize: JSX becomes h() calls and TS types are stripped", async () => {
  const src = `export function F(): unknown {
    const n: number = 1;
    return <div class="x"><text>hi {n}</text></div>;
  }`;
  const { code, ast } = await normalize(src, "f.tsx");
  assert.match(code, /h\("div"/);
  assert.match(code, /h\("text"/);
  assert.doesNotMatch(code, /:\s*number/); // types stripped
  assert.ok(ast, "returns a Babel AST");
});

test("normalize: produces a source map", async () => {
  const { map } = await normalize(`export function F(){ return <div></div>; }`, "f.tsx");
  assert.ok(map && Array.isArray(map.sources), "returns a v3 source map");
});

test("normalize: async/await is lowered to a Promise/.then chain", async () => {
  const src = `const f = async (x: number) => (await g(x)).y;`;
  const { code } = await normalize(src, "f.ts");
  assert.ok(!/\basync\b/.test(code) && !/\bawait\b/.test(code),
    `async/await should be lowered; got: ${code}`);
});
