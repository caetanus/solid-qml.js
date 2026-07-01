// A minimal DAP stub standing in for qml-debug's adapter, so the proxy can be tested without
// VS Code or a running Qt engine. It echoes back the source/line it RECEIVED on setBreakpoints
// (revealing the outbound rewrite), and logs what it got to stderr.
import process from "node:process";

function makeParser(onMessage) {
  let buf = Buffer.alloc(0);
  return (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      const sep = buf.indexOf("\r\n\r\n");
      if (sep < 0) return;
      const m = /Content-Length:\s*(\d+)/i.exec(buf.slice(0, sep).toString());
      const len = parseInt(m[1], 10);
      const start = sep + 4;
      if (buf.length < start + len) return;
      const body = buf.slice(start, start + len).toString();
      buf = buf.slice(start + len);
      onMessage(JSON.parse(body));
    }
  };
}
function write(msg) {
  const j = Buffer.from(JSON.stringify(msg));
  process.stdout.write(`Content-Length: ${j.length}\r\n\r\n`);
  process.stdout.write(j);
}

process.stdin.on("data", makeParser((msg) => {
  if (msg.command === "initialize") {
    write({ type: "response", request_seq: msg.seq, success: true, command: "initialize", body: {} });
    write({ type: "event", event: "initialized" });
  } else if (msg.command === "attach") {
    process.stderr.write(`INNER attach=${JSON.stringify(msg.arguments)}\n`);
    write({ type: "response", request_seq: msg.seq, success: true, command: "attach" });
  } else if (msg.command === "setBreakpoints") {
    const a = msg.arguments;
    process.stderr.write(`INNER setBreakpoints source=${a.source.path} line=${a.breakpoints[0].line}\n`);
    write({
      type: "response", request_seq: msg.seq, success: true, command: "setBreakpoints",
      body: { breakpoints: a.breakpoints.map((bp) => ({ verified: true, line: bp.line, source: a.source })) },
    });
  }
}));
