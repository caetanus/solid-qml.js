// <VirtualList>: a new `each` keeps the rows and the scroll position. The rows are a ListModel kept in
// step with `each` by key, each reading `__src[index]`: a republish with the same keys recreates no
// row (solid-mail: an open thread's rows unfolded again on every republish — a flicker), rows that
// came or went are inserted/removed, and the view stays on the row that was at its top.
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

test("VirtualList: rows are a ListModel synced by key; each row reads __src[index] (key id by default)", async () => {
  const out = await qml(list(""));
  assert.match(out, /property var __src: ts\n\s*property var __keys: \[\]\n\s*ListModel \{ id: (__vlist\d+)Rows \}\n\s*model: \1Rows/);
  assert.match(out, /on__SrcChanged: __swap\(\)\n\s*Component\.onCompleted: __swap\(\)/);
  assert.doesNotMatch(out, /\bmodel: ts\b/);
  assert.doesNotMatch(out, /model = /);   // never assigned: that reset the ListView
  assert.match(out, /keys\.push\(src\[n\] \? src\[n\]\.id : undefined\)/);
  assert.match(out, /property var __forItem: ListView\.view \? ListView\.view\.__src\[index\] : undefined/);
  assert.match(out, /text: "" \+ \(__for\d+\.__forItem\.from\)/);   // the body reads the published row
});

test("VirtualList key: a field name or accessor picks the field the position is kept by", async () => {
  for (const attrs of [` key="uid"`, ` key={(r) => r.uid}`]) {
    const out = await qml(list(attrs));
    assert.match(out, /keys\.push\(src\[n\] \? src\[n\]\.uid : undefined\)/, attrs);
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

test("VirtualList __swap in a real ListView: no row recreated on a republish, same top row, a row gone above, at the top", { skip: tool === null ? "no qml6" : false }, async () => {
  const out = await qml(list(""));
  const lines = out.split("\n");
  const from = lines.findIndex((l) => /property var __src:/.test(l));
  const to = lines.findIndex((l) => /Component\.onCompleted: __swap\(\)/.test(l));
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
    delegate: Rectangle {
      property var __forItem: ListView.view ? ListView.view.__src[index] : undefined
      width: lv.width; height: __forItem && __forItem.id % 3 ? 72 : 96
      Component.onCompleted: created++
    }
  }
  property int created: 0
  function topId() { return lv.__keys[lv.indexAt(10, lv.contentY + 10)] }
  Component.onCompleted: { try { run() } catch (e) { console.log("ERR " + e) } Qt.quit() }
  function run() {
    rows = make(-1); lv.contentY = 5000
    var t0 = topId(), y0 = lv.contentY, c0 = created
    var vi = lv.indexAt(10, lv.contentY + 10) + 1, before = lv.itemAtIndex(vi)
    rows = make(lv.__keys[vi])
    console.log("READ " + (topId() === t0) + " " + (lv.contentY === y0) + " " + (created - c0) + " " + (lv.itemAtIndex(vi) === before) + " " + before.__forItem.unread)
    rows = make(10, true)
    console.log("DROP " + (topId() === t0))
    lv.positionViewAtBeginning(); rows = make(5); lv.forceLayout()
    console.log("TOP " + lv.indexAt(10, lv.contentY + 1))
    c0 = created
    rows = [{ id: 5000, unread: true }].concat(make(5))
    lv.forceLayout()
    console.log("NEW " + (created - c0 <= 1) + " " + lv.__keys[0] + " " + lv.indexAt(10, lv.contentY + 1))
    rows = 5
    console.log("COUNT " + lv.count)
  }
}
`);
    // console.log goes to stderr: both streams, through the shell
    res = execFileSync("sh", ["-c", `"${tool}" "${join(dir, "t.qml")}" 2>&1`], {
      env: { ...process.env, QT_QPA_PLATFORM: "offscreen", QT_FORCE_STDERR_LOGGING: "1" }, encoding: "utf8", timeout: 20000,
    });
  } finally { rmSync(dir, { recursive: true, force: true }); }
  assert.match(res, /READ true true 0 true false/, res);   // same top row and y, no row created, same delegate, its data updated
  assert.match(res, /DROP true/, res);
  assert.match(res, /TOP 0/, res);
  assert.match(res, /NEW true 5000 0/, res);   // at the top: at most one row created (or reused from the pool), the view stays at the top
  assert.match(res, /COUNT 0/, res);      // a non-array each lists nothing
});
