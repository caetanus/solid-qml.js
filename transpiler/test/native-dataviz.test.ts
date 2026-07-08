/** Opt-in viz/media modules: each tag pulls ITS module import only when used (core stays clean). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitComponentType } from "../src/emit/component.ts";
import * as t from "@babel/types";

async function qmlType(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const file = ast as t.File;
  let fn: t.Function | null = null;
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node)) fn = node;
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  }
  if (!fn) throw new Error("no fn");
  const render = findRender(ast)!;
  return emitComponentType(fn, render, new Map()).join("\n");
}

test("dataviz: <MediaPlayer> instantiates the opt-in Media module with src/autoplay", async () => {
  const out = await qmlType(`export function F(){ return <MediaPlayer class="mp" src="assets/clip.mp4" autoplay />; }`);
  assert.match(out, /import solidqml\.Widgets\.Media 1\.0 as WMedia/);
  assert.match(out, /WMedia\.MediaPlayer \{/);
  assert.match(out, /cssClass: \["mp"\]/);
  assert.match(out, /src: Qt\.resolvedUrl\("assets\/clip\.mp4"\)/); // Qt 6: url strings resolve at USE (cwd) unless resolved at the author document
  assert.match(out, /autoplay: true/);
});

test("dataviz: an app without <MediaPlayer> never imports the Media module", async () => {
  const out = await qmlType(`export function F(){ return <div class="a" />; }`);
  assert.doesNotMatch(out, /solidqml\.Widgets\.Media/);
});

test("dataviz: <WebView> instantiates the opt-in Web module with a resolved src", async () => {
  const out = await qmlType(`export function F(){ return <WebView class="wv" src="assets/page.html" />; }`);
  assert.match(out, /import solidqml\.Widgets\.Web 1\.0 as WWeb/);
  assert.match(out, /WWeb\.WebView \{/);
  assert.match(out, /src: Qt\.resolvedUrl\("assets\/page\.html"\)/);
});

test("dataviz: <RichText> instantiates the opt-in RichText module", async () => {
  const out = await qmlType(`export function F(){ return <RichText class="ed" />; }`);
  assert.match(out, /import solidqml\.Widgets\.RichText 1\.0 as WRich/);
  assert.match(out, /WRich\.RichText \{/);
  assert.match(out, /cssClass: \["ed"\]/);
});

test("dataviz: <CodeEditor> instantiates the opt-in Code module with language + text", async () => {
  const out = await qmlType(`export function F(){ return <CodeEditor class="ed" language="QML" text={"a"} />; }`);
  assert.match(out, /import solidqml\.Widgets\.Code 1\.0 as WCode/);
  assert.match(out, /WCode\.CodeEditor \{/);
  assert.match(out, /language: "QML"/);
  assert.match(out, /text: "a"/);
});
