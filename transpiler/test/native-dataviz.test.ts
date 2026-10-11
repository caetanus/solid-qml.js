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

test("dataviz: <MediaPlayer subtitles> wires the resolved .srt url", async () => {
  const out = await qmlType(`export function F(){ return <MediaPlayer src="a.mp4" subtitles="a.srt" />; }`);
  assert.match(out, /subtitles: Qt\.resolvedUrl\("a\.srt"\)/);
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

test("dataviz: <WebView html> binds the HTML string and the remote-content switch", async () => {
  const out = await qmlType(`export function F(){ const [ok,setOk]=createSignal(false); const [h,setH]=createSignal("<p>x</p>"); return <WebView class="wv" html={h()} remoteContent={ok()} />; }`);
  assert.match(out, /WWeb\.WebView \{/);
  assert.match(out, /html: h\b/);
  assert.match(out, /remoteContent: ok\b/);
  assert.doesNotMatch(out, /src:/);
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

// ── <DocumentEditor>: the app drives the document (office item 0) ─────────────────────────────

test("document editor: <DocumentEditor> instantiates WRich.DocumentEditor from the RichText module", async () => {
  const out = await qmlType(`export function F(){ return <DocumentEditor class="editor" />; }`);
  assert.match(out, /import solidqml\.Widgets\.RichText 1\.0 as WRich/);
  assert.match(out, /WRich\.DocumentEditor \{/);
  assert.match(out, /cssClass: \["editor"\]/);
  assert.doesNotMatch(out, /variant:/); // the document variant IS the type
});

test("document editor: toolbar={false} hides the built-in toolbar; a dynamic value binds", async () => {
  const off = await qmlType(`export function F(){ return <DocumentEditor toolbar={false} />; }`);
  assert.match(off, /toolbar: false/);
  const dyn = await qmlType(`export function F(){ const [t,setT]=createSignal(true); return <DocumentEditor toolbar={t()} />; }`);
  assert.match(dyn, /toolbar: t\b/);
});

test("document editor: ref={doc} gives the editor its id; methods and format state resolve through it", async () => {
  const out = await qmlType(`export function F(){
    let doc;
    const [st,setSt]=createSignal("");
    const save = () => { const r = doc.saveOdf("/x/a.odt"); setSt(r.ok ? "saved" : r.error); };
    return <div>
      <button classList={{ on: doc.bold }} onClick={() => doc.toggleBold()}>B</button>
      <button onClick={save}>S</button>
      <DocumentEditor ref={doc} toolbar={false} />
    </div>; }`);
  assert.match(out, /WRich\.DocumentEditor \{\s*\n\s*id: _ref_doc/);
  assert.match(out, /_ref_doc\.toggleBold\(\)/);
  assert.match(out, /_ref_doc\.bold/);
  assert.match(out, /_ref_doc\.saveOdf\("\/x\/a\.odt"\)/);
});

test("document editor: onModifiedChanged receives the flag; onContentChanged takes no payload", async () => {
  const out = await qmlType(`export function F(){
    const [dirty,setDirty]=createSignal(false); const [n,setN]=createSignal(0);
    return <DocumentEditor onModifiedChanged={(m) => setDirty(m)} onContentChanged={() => setN(n() + 1)} />; }`);
  // Inside the instance a bare `modified` is the editor's own property (QML: the scope object's
  // properties come before the component root's).
  assert.match(out, /onModifiedChanged: \{[^\n]*dirty = modified \}/);
  assert.match(out, /onContentChanged: \{[^\n]*n = n \+ 1/);
});

test("document editor: <RichText> keeps its mail/forms contract (no toolbar/ref/onContentChanged)", async () => {
  await assert.rejects(qmlType(`export function F(){ return <RichText toolbar={false} />; }`), /<RichText> does not support `toolbar`/);
  await assert.rejects(qmlType(`export function F(){ return <DocumentEditor html="<p>x</p>" />; }`), /<DocumentEditor> does not support `html`/);
});
