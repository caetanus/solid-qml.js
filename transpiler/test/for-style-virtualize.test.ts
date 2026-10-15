// <For> rows with an inline `style` OBJECT are certified for virtualization (forCertify.ts) when the row's
// size stays predictable (owner rule): paint-only keys take any side-effect-free value (row, index,
// signals, memos, calls); layout keys take literals or values from the row's own record (measured per
// row by the list); anything else — or a style whose keys are unknown at compile time — stays eager,
// with the reason reported.
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
  return forRowCertification(delegate, {
    isReactiveRead: (n) => ["signal", "memo"].includes(table.get(n)?.kind ?? ""),
    isSetter: (n) => ["setter", "storeSetter"].includes(table.get(n)?.kind ?? ""),
  });
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

// ---- Accepted: paint-only keys, any side-effect-free value -------------------------------------

test("certified: paint-only key from the row item", () => accepted(`(row) => <div class="r" style={{ background: row.c }} />`));

test("certified: paint-only keys from a component signal, a memo and the row index", () =>
  accepted(`(row, i) => <div style={{ background: tone(), color: dim() ? "gray" : "black", opacity: i() % 2 ? 0.5 : 1 }}><text>{row.id}</text></div>`));

test("certified: paint-only keys from a helper call, an outer identifier and a template", () =>
  accepted("(row) => <div style={{ backgroundColor: fmt(row.c), borderColor: outer, boxShadow: `0 0 4px ${tone()}` }} />"));

test("certified: every paint-only family (border colour/style/radius, outline, shadows, transform, visibility)", () =>
  accepted(`(row) => <div style={{ borderTopColor: tone(), borderStyle: row.s, borderRadius: fmt(4), outline: tone(),
    outlineWidth: row.w, textShadow: tone(), transform: row.t, visibility: dim() ? "hidden" : "visible", textDecoration: row.d }} />`));

// ---- Accepted: layout keys with a fixed or row-measured value ---------------------------------

test("certified: layout keys with literal values (a fixed size)", () =>
  accepted(`(row) => <div style={{ width: 40, height: "20px", marginTop: -2, padding: "2px 4px", fontSize: 12, "--gap": 4, flexGrow: 0 }} />`));

test("certified: layout keys from the row's own record (measured per row, like class={row.status})", () =>
  accepted("(row) => <div style={{ height: row.h, width: `${row.w}px`, padding: row.big ? 8 : 2, fontSize: row.s + 1 }} />"));

test("certified: a paint key from a signal next to a layout key from the row", () =>
  accepted(`(row) => <div style={{ background: tone(), height: row.h }} />`));

test("certified: a static style string and a template without interpolation", async () => {
  await accepted(`(row) => <div style="height: 20px; color: red" />`);
  await accepted("(row) => <div style={`height: 20px`} />");
});

test("certified: style on nested text tags of the row", () =>
  accepted(`(row) => <div class="r"><span style={{ color: tone(), fontSize: 12 }}>{row.label}</span><p style={{ fontWeight: row.w }}>x</p></div>`));

// ---- Rejected: a layout key whose value would make the size unpredictable ----------------------

test("not certified: a layout key from a component signal or memo", async () => {
  await rejected(`(row) => <div style={{ width: tone() }} />`,
    /^style\.width: layout property depends on the component signal\/memo `tone\(\)`/);
  await rejected(`(row) => <div style={{ background: tone(), fontSize: dim() ? 12 : 14 }} />`,
    /^style\.font-size: layout property depends on the component signal\/memo `dim\(\)`/);
});

test("not certified: a layout key from the row index", () =>
  rejected(`(row, i) => <div style={{ height: i() * 2 }} />`, /^style\.height: layout property depends on the row index `i\(\)`/));

test("not certified: a layout key from a call or an outer identifier", async () => {
  await rejected(`(row) => <div style={{ marginTop: fmt(row.m) }} />`, /^style\.margin-top: layout property depends on a call to `fmt`/);
  await rejected(`(row) => <div style={{ padding: outer }} />`, /^style\.padding: layout property depends on `outer`/);
});

test("not certified: unknown keys, custom properties and the border shorthand count as layout", async () => {
  await rejected(`(row) => <div style={{ "--accent": tone() }} />`, /^style\.--accent: layout property depends on the component signal/);
  await rejected(`(row) => <div style={{ cursor: tone() }} />`, /^style\.cursor: layout property depends on/);
  await rejected(`(row) => <div style={{ border: tone() }} />`, /^style\.border: layout property depends on/);
});

// ---- Rejected: side effects, and styles whose properties are not known at compile time ---------

test("not certified: a side effect in a paint-only value", async () => {
  await rejected(`(row) => <div style={{ background: setTone("red") }} />`, /^style\.background: a side effect \(the setter `setTone`\)$/);
  await rejected(`(row) => <div style={{ color: (row.n = 1) }} />`, /^style\.color: a side effect \(AssignmentExpression\)$/);
  await rejected(`(row) => <div style={{ opacity: row.n++ }} />`, /^style\.opacity: a side effect \(UpdateExpression\)$/);
});

test("not certified: a spread or a computed key in the style object", async () => {
  await rejected(`(row) => <div style={{ ...row.style, color: "red" }} />`, /^style: a spread in the style object — its properties are not known at compile time$/);
  await rejected(`(row) => <div style={{ [row.k]: "red" }} />`, /^style: a computed key in the style object/);
});

test("not certified: a style that is not an object literal (unknown keys)", async () => {
  for (const style of ["{row.style}", `{row.on ? "color: red" : "color: blue"}`, "{`background: ${row.c}`}"])
    await rejected(`(row) => <div style=${style} />`, /^style: not an object literal — its properties are not known at compile time$/);
});

test("not certified: a method in the style object", () =>
  rejected(`(row) => <div style={{ color() { return "red"; } }} />`, /^style: a ObjectMethod in the style object$/));

test("not certified: a signal read stays rejected in class and text (only style is classified by key)", async () => {
  await rejected(`(row) => <div class={tone()} />`, /^class: a call to `tone`/);
  await rejected(`(row) => <text>{tone()}</text>`, /^child: a call to `tone`/);
  await rejected(`(row, i) => <text>{i()}</text>`, /^child: a call to `i`/);
});

test("not certified: an event handler next to the style (the office colour swatch shape)", () =>
  rejected(`(c) => <div class="swatch" style={{ background: c }} onClick={() => setTone(c)} />`, /^attribute `onClick`$/));

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
