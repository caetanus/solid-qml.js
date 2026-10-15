// <For> rows with an inline `style` OBJECT are certified for virtualization (forCertify.ts) when every
// value is pure: the row item, the row index accessor i(), literals, component signal/memo reads, and
// operators over those. A virtualized row is created, parked and rebound as it scrolls, so its bindings
// must be re-evaluable with no side effect; anything else stays eager, with the reason reported.
// The app-level counterpart (2000 rows, recycled colours, a live signal) is tests/for-style-virtual/.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { isHCall } from "../src/ast/h.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { forRowCertification } from "../src/emit/forCertify.ts";
import type { Scope } from "../src/emit/expr.ts";

const wrap = (row: string, setup = "") =>
  `import { For, createSignal, createMemo } from "solid-js";
   export function C(){ const [rows,setRows]=createSignal([]); const [tone,setTone]=createSignal("#fff");
     const dim = createMemo(() => tone() === "#000"); function fmt(x) { return x; } const outer = "red"; ${setup}
     return <div class="list"><For each={rows()}>{${row}}</For></div>; }`;

async function parse(src: string): Promise<{ fn: t.Function; render: t.CallExpression }> {
  const { ast } = await normalize(src, "f.tsx");
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body)
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  return { fn: fn!, render: findRender(ast)! };
}

async function qml(src: string): Promise<string> {
  const { fn, render } = await parse(src);
  const scope: Scope = { table: analyzeSignals(fn), mode: "binding" };
  return emitQml(render, scope).join("\n");
}

/** The certifier's verdict on the (only) <For> row of `row`, with the component's signals/memos known. */
async function verdict(row: string, setup = "") {
  const { fn } = await parse(wrap(row, setup));
  const table = analyzeSignals(fn);
  let delegate: t.ArrowFunctionExpression | null = null;
  t.traverseFast(fn, (n) => { if (!delegate && t.isArrowFunctionExpression(n) && isHCall(n.body)) delegate = n; });
  assert.ok(delegate, "no row delegate found");
  return forRowCertification(delegate, { isReactiveRead: (n) => ["signal", "memo"].includes(table.get(n)?.kind ?? "") });
}

async function accepted(row: string, setup = "") {
  const v = await verdict(row, setup);
  assert.deepEqual(v, { ok: true }, row);
  assert.match(await qml(wrap(row, setup)), /^\s*model: rows\n\s*virtualize: true$/m, row);
}

async function rejected(row: string, reason: RegExp, setup = "") {
  const v = await verdict(row, setup);
  assert.equal(v.ok, false, row);
  assert.match((v as { reason: string }).reason, reason, row);
  assert.doesNotMatch(await qml(wrap(row, setup)), /virtualize/, row);
}

// ---- Accepted shapes ---------------------------------------------------------------------------

test("certified: style object from the row item", () => accepted(`(row) => <div class="r" style={{ background: row.c }} />`));

test("certified: style object with literals (numbers get px, negative numbers, strings, booleans)", () =>
  accepted(`(row) => <div style={{ width: 40, marginTop: -2, opacity: 0.5, color: "red", "--flag": true, border: null }} />`));

test("certified: style object from the row index accessor", () =>
  accepted(`(row, i) => <div style={{ opacity: i() % 2 === 0 ? 1 : 0.5, order: i() }}><text>{row.id}</text></div>`));

test("certified: style object from a component signal and a memo", () =>
  accepted(`(row) => <div style={{ background: row.c, color: tone(), fontWeight: dim() ? 400 : 700 }} />`));

test("certified: template literals and string concatenation in style values", () =>
  accepted("(row) => <div style={{ width: `${row.w}px`, background: \"#\" + row.hex, border: `1px solid ${tone()}` }} />"));

test("certified: logical and comparison operators in style values", () =>
  accepted(`(row) => <div style={{ color: row.c || "black", opacity: row.n > 3 && !row.off ? 1 : 0.4 }} />`));

test("certified: quoted keys and custom properties", () =>
  accepted(`(row) => <div style={{ "background-color": row.c, "--accent": tone() }} />`));

test("certified: a static style string, a dynamic template string, and a runtime style value", async () => {
  await accepted(`(row) => <div style="color: red" />`);
  await accepted("(row) => <div style={`background: ${row.c}`} />");
  await accepted(`(row) => <div style={row.style} />`);
  await accepted(`(row) => <div style={row.on ? "color: red" : "color: blue"} />`);
});

test("certified: style on nested text tags of the row", () =>
  accepted(`(row) => <div class="r"><span style={{ color: row.c }}>{row.label}</span><p style={{ fontSize: 12 }}>x</p></div>`));

// ---- Rejected shapes, with their reasons -------------------------------------------------------

test("not certified: a call to a helper in a style value", () =>
  rejected(`(row) => <div style={{ background: fmt(row.c) }} />`, /^style\.background: a call to `fmt`/));

test("not certified: a call with arguments, even to a signal", () =>
  rejected(`(row) => <div style={{ color: tone(row.c) }} />`, /^style\.color: a call to `tone`/));

test("not certified: an outer (closed-over) identifier in a style value", () =>
  rejected(`(row) => <div style={{ color: outer }} />`, /^style\.color: `outer`/));

test("not certified: the bare (uncalled) index accessor", () =>
  rejected(`(row, i) => <div style={{ order: i }} />`, /^style\.order: `i`/));

test("not certified: a spread in the style object", () =>
  rejected(`(row) => <div style={{ ...row.style, color: "red" }} />`, /^style: a spread in the style object$/));

test("not certified: a computed key in the style object", () =>
  rejected(`(row) => <div style={{ [row.k]: "red" }} />`, /^style: a computed key in the style object$/));

test("not certified: a method or a function value in the style object", async () => {
  await rejected(`(row) => <div style={{ color() { return "red"; } }} />`, /^style: a ObjectMethod in the style object$/);
  await rejected(`(row) => <div style={{ color: () => row.c }} />`, /^style\.color: a ArrowFunctionExpression/);
});

test("not certified: a computed member access on the row", () =>
  rejected(`(row) => <div style={{ color: row[tone()] }} />`, /^style\.color: a MemberExpression/));

test("not certified: an assignment or `in` in a style value", async () => {
  await rejected(`(row) => <div style={{ color: (row.n = 1) }} />`, /^style\.color: a AssignmentExpression/);
  await rejected(`(row) => <div style={{ opacity: "x" in row ? 1 : 0 }} />`, /^style\.opacity: a BinaryExpression/);
});

test("not certified: a signal read stays rejected in class and text (style alone widened)", async () => {
  await rejected(`(row) => <div class={tone()} />`, /^class: a call to `tone`/);
  await rejected(`(row) => <text>{tone()}</text>`, /^child: a call to `tone`/);
  await rejected(`(row, i) => <text>{i()}</text>`, /^child: a call to `i`/);
});

test("not certified: an event handler next to the style (the office colour swatch shape)", () =>
  rejected(`(c) => <div class="swatch" style={{ background: c }} onClick={() => setTone(c)} />`, /^attribute `onClick`$/));

test("not certified: a row param that shadows a signal name is not a signal read", () =>
  rejected(`(tone) => <div style={{ color: tone() }} />`, /^style\.color: a call to `tone`/));

test("not certified: three delegate parameters", async () => {
  const v = await verdict(`(row, i, extra) => <div style={{ color: row.c }} />`);
  assert.deepEqual(v, { ok: false, reason: "the row function takes 1 or 2 parameters" });
});

// ---- Emitted QML ------------------------------------------------------------------------------

test("emitted QML: a virtualized row binds its inline style to modelData, __forIndex and the signal", async () => {
  const out = await qml(wrap(`(row, i) => <div class="r" style={{ background: row.c, color: tone(), opacity: i() % 2 ? 0.5 : 1, width: 40 }} />`));
  const expected = [
    "    Css.CssRepeater {",
    "        model: rows",
    "        virtualize: true",
    "        delegate: Component {",
    "            W.Div {",
    "                property int __forIndex: index",
    "                cssClass: [\"r\"]",
    "                inlineStyle: ({ \"background\": modelData.c, \"color\": tone, \"opacity\": __forIndex % 2 ? 0.5 : 1, \"width\": \"40px\" })",
    "            }",
    "        }",
    "    }",
  ].join("\n");
  assert.ok(out.includes(expected), out);
});
