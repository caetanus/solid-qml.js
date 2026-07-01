#!/usr/bin/env node
import { spawn } from "node:child_process";
import path from "node:path";
import process from "node:process";
import { loadSourceMap } from "../../scripts/sourcemap.mjs";
import { rewriteToInner, rewriteToClient } from "./rewrite.mjs";

// Transparent DAP proxy: VS Code <-> (this) <-> qml-debug's adapter <-> Qt V4 engine.
// It forwards every DAP message untouched EXCEPT (a) the attach/launch config — our fields
// (tsxSource/generatedQml/sourceMap) are translated to qml-debug's (host/port/paths) — and
// (b) source locations, which are remapped main.tsx<->App.generated.qml via the source map.
// `seq`/`request_seq` are preserved, so the conversation stays in lock-step.
//
// Usage: node proxy.mjs --inner /path/to/qml-debug/out/debug-adapter.js

function argValue(flag) {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const innerAdapter = argValue("--inner");
if (!innerAdapter) {
  process.stderr.write("qml-tsx-dap: missing --inner <path to qml-debug debug-adapter.js>\n");
  process.exit(2);
}

const log = (m) => process.stderr.write(`[qml-tsx-dap] ${m}\n`);

// --- DAP framing: `Content-Length: N\r\n\r\n<json>` --------------------------------------
function makeParser(onMessage) {
  let buf = Buffer.alloc(0);
  return (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      const sep = buf.indexOf("\r\n\r\n");
      if (sep < 0) return;
      const header = buf.slice(0, sep).toString("utf8");
      const m = /Content-Length:\s*(\d+)/i.exec(header);
      if (!m) { buf = buf.slice(sep + 4); continue; }
      const len = parseInt(m[1], 10);
      const start = sep + 4;
      if (buf.length < start + len) return; // wait for the full body
      const body = buf.slice(start, start + len).toString("utf8");
      buf = buf.slice(start + len);
      try { onMessage(JSON.parse(body)); } catch (e) { log(`bad message: ${e}`); }
    }
  };
}

function write(stream, msg) {
  const json = Buffer.from(JSON.stringify(msg), "utf8");
  stream.write(`Content-Length: ${json.length}\r\n\r\n`);
  stream.write(json);
}

// --- proxy state --------------------------------------------------------------------------
let ctx = null; // { tsxPath, qmlPath, sm } once attach/launch is seen

// Translate our launch/attach config into what qml-debug expects, and capture the mapping.
function configureFromAttach(args) {
  if (!args) return;
  const tsxPath = args.tsxSource && path.resolve(args.tsxSource);
  const qmlPath = args.generatedQml && path.resolve(args.generatedQml);
  const mapPath = args.sourceMap
    ? path.resolve(args.sourceMap)
    : (qmlPath ? `${qmlPath}.map` : undefined);
  if (tsxPath && qmlPath && mapPath) {
    try { ctx = { tsxPath, qmlPath, sm: loadSourceMap(mapPath) }; log(`mapping ${path.basename(tsxPath)} <-> ${path.basename(qmlPath)}`); }
    catch (e) { log(`could not load source map ${mapPath}: ${e}`); }
  }
  // qml-debug speaks: host, port, paths. Strip our extra fields so it isn't confused.
  delete args.tsxSource;
  delete args.generatedQml;
  delete args.sourceMap;
}

const inner = spawn(process.execPath, [innerAdapter, ...process.argv.slice(process.argv.indexOf("--inner") + 2)], {
  stdio: ["pipe", "pipe", "pipe"],
});
inner.on("exit", (code) => { log(`inner adapter exited (${code})`); process.exit(code ?? 0); });
inner.stderr.on("data", (d) => process.stderr.write(d));

// VS Code -> inner
const fromClient = makeParser((msg) => {
  if (msg.type === "request" && (msg.command === "attach" || msg.command === "launch"))
    configureFromAttach(msg.arguments);
  if (ctx) rewriteToInner(msg, ctx);
  write(inner.stdin, msg);
});
process.stdin.on("data", fromClient);
process.stdin.on("end", () => inner.stdin.end());

// inner -> VS Code
const fromInner = makeParser((msg) => {
  if (ctx) rewriteToClient(msg, ctx);
  write(process.stdout, msg);
});
inner.stdout.on("data", fromInner);

log(`proxy up; inner=${innerAdapter}`);
