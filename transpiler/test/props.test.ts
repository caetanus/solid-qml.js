import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { analyzeProps } from "../src/model/symbols.ts";
import { emitExpr } from "../src/emit/expr.ts";

async function fnOf(src: string): Promise<t.Function> {
  const { ast } = await normalize(src, "f.tsx");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  return fn!;
}
async function expr(code: string): Promise<t.Expression> {
  const { ast } = await normalize(`const __x = (${code});`, "e.tsx");
  let found: t.Expression | null = null;
  for (const node of (ast as t.File).program.body) if (t.isVariableDeclaration(node)) found = node.declarations[0].init as t.Expression;
  return found!;
}

test("analyzeProps: finds the props param name and the props used", async () => {
  const fn = await fnOf(`export function Card(props){ return <text>{props.label}</text>; }`);
  const info = analyzeProps(fn);
  assert.equal(info.param, "props");
  assert.deepEqual(info.used, ["label"]);
});

test("emitExpr: props.X resolves to the bare property X", async () => {
  assert.equal(emitExpr(await expr("props.label"), { table: new Map(), mode: "binding", propsParam: "props" }), "label");
});

test("emitExpr: props.todo.done (nested member) resolves to 'todo.done' in binding mode", async () => {
  assert.equal(
    emitExpr(await expr("props.todo.done"), { table: new Map(), mode: "binding", propsParam: "props" }),
    "todo.done",
  );
});

test("emitExpr: props.todo.id resolves to 'todo.id' in binding mode", async () => {
  assert.equal(
    emitExpr(await expr("props.todo.id"), { table: new Map(), mode: "binding", propsParam: "props" }),
    "todo.id",
  );
});

test("emitExpr: props.onToggle(props.todo.id) resolves to 'onToggle(todo.id)' in handler mode", async () => {
  assert.equal(
    emitExpr(await expr("props.onToggle(props.todo.id)"), { table: new Map(), mode: "handler", propsParam: "props" }),
    "onToggle(todo.id)",
  );
});

test("emitExpr: props.todo.title resolves to 'todo.title' in binding mode", async () => {
  assert.equal(
    emitExpr(await expr("props.todo.title"), { table: new Map(), mode: "binding", propsParam: "props" }),
    "todo.title",
  );
});
