import path from "node:path";

// Pure DAP-message rewriting for the TSX↔QML debug proxy. The proxy sits between VS Code and
// the (unmodified) qml-debug adapter; here we translate the source locations that cross it:
// outbound (VS Code→qml-debug) main.tsx → App.generated.qml, inbound (qml-debug→VS Code) back.
// All DAP line/column numbers are 1-based; the source map is 0-based.

function samePath(a, b) {
  if (!a || !b) return false;
  return path.resolve(a) === path.resolve(b);
}

// main.tsx line (1-based) → App.generated.qml line (1-based).
function tsxToQmlLine(sm, line1) {
  const gen = sm.sourceToGenerated(line1 - 1);
  return gen == null ? line1 : gen + 1;
}

// App.generated.qml line (1-based) → main.tsx line (1-based).
function qmlToTsxLine(sm, line1) {
  const src = sm.generatedToSource(line1 - 1);
  return src ? src.srcLine + 1 : line1;
}

function tsxSource(ctx) {
  return { name: path.basename(ctx.tsxPath), path: ctx.tsxPath };
}
function qmlSource(ctx) {
  return { name: path.basename(ctx.qmlPath), path: ctx.qmlPath };
}

// VS Code → qml-debug. Rewrite breakpoint requests from the TSX source/lines to the generated
// QML source/lines so qml-debug sets them where the engine actually runs.
export function rewriteToInner(msg, ctx) {
  if (msg.type !== "request" || !msg.arguments) return msg;
  const a = msg.arguments;
  if ((msg.command === "setBreakpoints" || msg.command === "breakpointLocations")
      && a.source && samePath(a.source.path, ctx.tsxPath)) {
    a.source = qmlSource(ctx);
    if (Array.isArray(a.breakpoints))
      for (const bp of a.breakpoints) if (typeof bp.line === "number") bp.line = tsxToQmlLine(ctx.sm, bp.line);
    if (Array.isArray(a.lines)) // legacy field
      a.lines = a.lines.map((l) => tsxToQmlLine(ctx.sm, l));
    if (typeof a.line === "number") a.line = tsxToQmlLine(ctx.sm, a.line);
    if (typeof a.endLine === "number") a.endLine = tsxToQmlLine(ctx.sm, a.endLine);
  }
  return msg;
}

// qml-debug → VS Code. Rewrite any generated-QML location back to the TSX source/line, so VS
// Code shows breakpoints, the stack, and stop locations in the file the developer wrote.
export function rewriteToClient(msg, ctx) {
  const isQml = (src) => src && samePath(src.path, ctx.qmlPath);
  const fixFrameLike = (o) => {
    if (o && isQml(o.source)) {
      if (typeof o.line === "number") o.line = qmlToTsxLine(ctx.sm, o.line);
      o.source = tsxSource(ctx);
    }
  };

  if (msg.type === "response" && msg.body) {
    const b = msg.body;
    if (msg.command === "stackTrace" && Array.isArray(b.stackFrames)) b.stackFrames.forEach(fixFrameLike);
    else if (msg.command === "setBreakpoints" && Array.isArray(b.breakpoints)) b.breakpoints.forEach(fixFrameLike);
    else if (msg.command === "breakpointLocations" && Array.isArray(b.breakpoints))
      for (const bp of b.breakpoints) if (typeof bp.line === "number") bp.line = qmlToTsxLine(ctx.sm, bp.line);
    else if (msg.command === "loadedSources" && Array.isArray(b.sources))
      b.sources.forEach((s) => { if (isQml(s)) Object.assign(s, tsxSource(ctx)); });
  } else if (msg.type === "event" && msg.body) {
    const b = msg.body;
    if (msg.event === "breakpoint") fixFrameLike(b.breakpoint);
    else if (msg.event === "loadedSource") { if (isQml(b.source)) b.source = tsxSource(ctx); }
    else if (msg.event === "output") fixFrameLike(b);
    else if (msg.event === "stopped" && b.source) fixFrameLike(b);
  }
  return msg;
}

export const _internals = { samePath, tsxToQmlLine, qmlToTsxLine };
