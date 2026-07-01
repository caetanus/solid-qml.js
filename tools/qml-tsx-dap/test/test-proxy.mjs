// Drives the proxy like VS Code would (initialize, attach, setBreakpoints), with a fake inner
// adapter, and checks both rewrite directions end-to-end (framing + spawn + translation).
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../../..");
const proxy = path.join(here, "..", "proxy.mjs");
const fakeInner = path.join(here, "fake-inner.mjs");

const p = spawn(process.execPath, [proxy, "--inner", fakeInner], { stdio: ["pipe", "pipe", "pipe"] });

let out = Buffer.alloc(0);
const clientMsgs = [];
function parseOut(chunk) {
  out = Buffer.concat([out, chunk]);
  for (;;) {
    const sep = out.indexOf("\r\n\r\n");
    if (sep < 0) return;
    const m = /Content-Length:\s*(\d+)/i.exec(out.slice(0, sep).toString());
    const len = parseInt(m[1], 10);
    const start = sep + 4;
    if (out.length < start + len) return;
    clientMsgs.push(JSON.parse(out.slice(start, start + len).toString()));
    out = out.slice(start + len);
  }
}
p.stdout.on("data", parseOut);
let innerLog = "";
p.stderr.on("data", (d) => (innerLog += d.toString()));

function send(msg) {
  const j = Buffer.from(JSON.stringify(msg));
  p.stdin.write(`Content-Length: ${j.length}\r\n\r\n`);
  p.stdin.write(j);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const tsx = path.join(root, "src/main.tsx");
const qml = path.join(root, "qml/solidqml/App.generated.qml");

send({ seq: 1, type: "request", command: "initialize", arguments: { linesStartAt1: true } });
await sleep(120);
send({ seq: 2, type: "request", command: "attach", arguments: { host: "127.0.0.1", port: 12150, tsxSource: tsx, generatedQml: qml } });
await sleep(120);
send({ seq: 3, type: "request", command: "setBreakpoints", arguments: { source: { path: tsx, name: "main.tsx" }, breakpoints: [{ line: 28 }] } });
await sleep(200);

p.kill();

const innerGotQml = /INNER setBreakpoints source=(\S+) line=(\d+)/.exec(innerLog);
const bpResp = clientMsgs.find((m) => m.command === "setBreakpoints" && m.type === "response");
const fr = bpResp && bpResp.body.breakpoints[0];

const okOut = innerGotQml && path.basename(innerGotQml[1]) === "App.generated.qml" && innerGotQml[2] === "67";
const okBack = fr && path.basename(fr.source.path) === "main.tsx" && fr.line === 28;
const sawInitialized = clientMsgs.some((m) => m.event === "initialized");

console.log("inbound (initialized forwarded):", sawInitialized ? "OK" : "FAIL");
console.log("outbound  tsx:28 -> inner:", innerGotQml ? `${path.basename(innerGotQml[1])}:${innerGotQml[2]}` : "FAIL", okOut ? "OK" : "FAIL");
console.log("inbound   inner -> client:", fr ? `${path.basename(fr.source.path)}:${fr.line}` : "FAIL", okBack ? "OK" : "FAIL");
process.exit(okOut && okBack && sawInitialized ? 0 : 1);
