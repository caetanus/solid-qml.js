// <VirtualList>: a new `each` keeps the scroll position — the row at the top of the view, by its key —
// instead of the ListView's reset to the top (solid-mail: a message marked read republished the list
// and the view jumped away).
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import type { Scope } from "../src/emit/expr.ts";

async function qml(src: string): Promise<string> {
  const { ast } = await normalize(src, "f.tsx");
  const render = findRender(ast)!;
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body) {
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
    if (t.isFunctionDeclaration(node)) fn = node;
  }
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding" };
  return emitQml(render, scope).join("\n");
}

const list = (attrs: string) =>
  `export function C(){ const [ts,setTs]=createSignal([]); return <VirtualList class="rows" each={ts()}${attrs}>{(t) => <div class="row"><text>{t.from}</text></div>}</VirtualList>; }`;

test("VirtualList: `each` goes through __src/__swap, not straight into `model` (keyed by id by default)", async () => {
  const out = await qml(list(""));
  assert.match(out, /property var __src: ts\n\s*function __swap\(\) \{/);
  assert.match(out, /on__SrcChanged: __swap\(\)\n\s*Component\.onCompleted: model = __src/);
  assert.doesNotMatch(out, /\bmodel: ts\b/);
  assert.match(out, /model\[r\]\.id !== undefined\) \{ k = model\[r\]\.id;/);
  assert.match(out, /src\[j\]\.id === k\) \{ positionViewAtIndex\(j, ListView\.Beginning\)/);
  assert.match(out, /if \(!Array\.isArray\(src\)\) \{ model = src; return \}/); // a count / ListModel: as it is
});

test("VirtualList key: a field name or accessor picks the field the position is kept by", async () => {
  for (const attrs of [` key="uid"`, ` key={(r) => r.uid}`]) {
    const out = await qml(list(attrs));
    assert.match(out, /model\[r\]\.uid !== undefined\) \{ k = model\[r\]\.uid;/, attrs);
    assert.match(out, /src\[j\]\.uid === k\)/, attrs);
    assert.doesNotMatch(out, /\.id\b/, attrs);
  }
  await assert.rejects(qml(list(` key={(r) => r.a + r.b}`)), /key must be a field name/);
});

// The emitted lines run in a real ListView (qml6, offscreen): skipped where Qt's `qml` tool is absent.
function qmlTool(): string | null {
  for (const exe of ["qml6", "/usr/lib/qt6/bin/qml"]) {
    try { execFileSync(exe, ["--help"], { stdio: "ignore", timeout: 5000 }); return exe; } catch { /* next */ }
  }
  return null;
}
const tool = qmlTool();

test("VirtualList __swap in a real ListView: same top row after a republish, a row gone above, at the top", { skip: tool === null ? "no qml6" : false }, async () => {
  const out = await qml(list(""));
  const lines = out.split("\n");
  const from = lines.findIndex((l) => /property var __src:/.test(l));
  const to = lines.findIndex((l) => /Component\.onCompleted: model = __src/.test(l));
  assert.ok(from >= 0 && to > from, out);
  const swap = lines.slice(from, to + 1).join("\n").replace(/property var __src: ts/, "property var __src: rows");
  const dir = mkdtempSync(join(tmpdir(), "vlist-"));
  let res = "";
  try {
    writeFileSync(join(dir, "t.qml"), `import QtQuick
Item {
  width: 400; height: 600
  property var rows: []
  function make(readId, drop) { var a = []; for (var i = 0; i < 200; i++) if (!(drop && i === readId)) a.push({ id: i, unread: i !== readId }); return a }
  ListView {
    id: lv; anchors.fill: parent; reuseItems: true; cacheBuffer: 600
${swap}
    delegate: Rectangle { width: lv.width; height: modelData.id % 3 ? 72 : 96 }
  }
  function topId() { return lv.model[lv.indexAt(10, lv.contentY + 10)].id }
  Component.onCompleted: { try { run() } catch (e) { console.log("ERR " + e) } Qt.quit() }
  function run() {
    rows = make(-1); lv.contentY = 5000
    var t0 = topId(), y0 = lv.contentY
    rows = make(70)
    console.log("READ " + (topId() === t0) + " " + (lv.contentY === y0))
    rows = make(10, true)
    console.log("DROP " + (topId() === t0))
    lv.contentY = 0; rows = make(5)
    console.log("TOP " + lv.contentY)
    lv.model = 3; rows = 5
    console.log("COUNT " + lv.count)
  }
}
`);
    // console.log goes to stderr: both streams, through the shell
    res = execFileSync("sh", ["-c", `"${tool}" "${join(dir, "t.qml")}" 2>&1`], {
      env: { ...process.env, QT_QPA_PLATFORM: "offscreen", QT_FORCE_STDERR_LOGGING: "1" }, encoding: "utf8", timeout: 20000,
    });
  } finally { rmSync(dir, { recursive: true, force: true }); }
  assert.match(res, /READ true true/, res);
  assert.match(res, /DROP true/, res);
  assert.match(res, /TOP 0/, res);
  assert.match(res, /COUNT 5/, res);
});
