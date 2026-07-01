import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";

test("findRender: returns the h() call returned by the exported component", async () => {
  const { ast } = await normalize(
    `export function F(){ return <div class="x"></div>; }`, "f.tsx");
  const call = findRender(ast);
  assert.ok(call && t.isCallExpression(call), "found a CallExpression");
  assert.ok(t.isIdentifier(call.callee, { name: "h" }), "it is an h() call");
  assert.ok(t.isStringLiteral(call.arguments[0], { value: "div" }), "first arg is 'div'");
});

test("findRender: returns null when the component returns no JSX", async () => {
  const { ast } = await normalize(`export function F(){ return 1; }`, "f.tsx");
  assert.equal(findRender(ast), null);
});
