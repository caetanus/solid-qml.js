// Sweep: native tag registry (+ HTML tags + builtins) × attribute names. Every attribute is either
// EMITTED (its value reaches the QML) or REPORTED (transpile error, or a warning for the documented
// web-only allow-list) — never silently ignored. Also proves the contract table (emit/attrs.ts)
// honest: a declared attribute must really be emitted.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as t from "@babel/types";
import { normalize } from "../src/babel/transform.ts";
import { findRender } from "../src/ast/find.ts";
import { analyzeSignals } from "../src/model/symbols.ts";
import { emitQml } from "../src/emit/qml.ts";
import { nativeTags } from "../src/emit/native/index.ts";
import { BUILTIN_TAGS } from "../src/ast/h.ts";
import { ELEMENT_ATTRS, REQUIRES, isWarnOnlyAttr, resetAttrWarnings, type AttrSpec } from "../src/emit/attrs.ts";
import type { Scope } from "../src/emit/expr.ts";

type Result = { out?: string; err?: string; warned: boolean };

async function emit(body: string): Promise<Result> {
  const src = `import { createSignal, Show, For, Index, Switch, Match, Suspense } from "solid-js"; import { Dynamic } from "solid-js/web";
    export function C(){ const [s,setS]=createSignal(0); let mm; return <div>${body}</div>; }`;
  const { ast } = await normalize(src, "c.tsx");
  const render = findRender(ast)!;
  let fn: t.Function | null = null;
  for (const node of (ast as t.File).program.body)
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration)) fn = node.declaration;
  const scope: Scope = { table: fn ? analyzeSignals(fn) : new Map(), mode: "binding", usedWidgets: {} as Scope["usedWidgets"] };
  resetAttrWarnings();
  let warned = false;
  const ow = console.warn;
  console.warn = () => { warned = true; };
  try { return { out: emitQml(render, scope).join("\n"), warned }; }
  catch (e) { return { err: (e as Error).message, warned }; }
  finally { console.warn = ow; }
}

/** One fixture per contract key: how to place the element (`wrap`), its children, and the
 *  attributes it needs to be valid at all (`base`). `{A}` marks where the attribute under test goes. */
type Fx = { key: string; el: string; base?: (attr: string) => string };
const el = (tag: string, kids = "x") => kids ? `<${tag} {A}>${kids}</${tag.split(" ")[0]}>` : `<${tag} {A} />`;
const FIXTURES: Fx[] = [
  { key: "box", el: el("div") }, { key: "box", el: el("section") }, { key: "box", el: el("a") },
  { key: "text", el: el("text") }, { key: "text", el: el("span") }, { key: "text", el: el("h1") },
  { key: "button", el: el("button") }, { key: "img", el: el("img", "") },
  { key: "input:text", el: el("input", "") }, { key: "input:text", el: el(`input type="password"`, "") },
  ...["checkbox", "radio", "range", "number", "date"].map((ty) => ({ key: `input:${ty}`, el: el(`input type="${ty}"`, "") })),
  { key: "textarea", el: el("textarea", "") },
  { key: "select", el: el("select", `<option value="a">A</option>`) },
  { key: "option", el: `<select>${el("option", "A")}</select>` },
  { key: "progress", el: el("progress", "") }, { key: "fieldset", el: el("fieldset") },
  { key: "legend", el: `<fieldset>${el("legend", "L")}<text>b</text></fieldset>` },
  { key: "dialog", el: el("dialog") }, { key: "details", el: el("details", "<summary>s</summary><text>b</text>") },
  { key: "summary", el: `<details>${el("summary", "S")}<text>b</text></details>` },
  { key: "StackView", el: el("StackView", "<div>a</div><div>b</div>") },
  { key: "MenuBar>Menu", el: `<MenuBar>${el("Menu", "<MenuItem onClick={() => setS(1)}>i</MenuItem>")}</MenuBar>` },
  ...["ToolBar", "SplitView", "Drawer", "SwipeView", "PageIndicator", "RangeSlider", "Dial", "Tumbler",
    "DelayButton", "BusyIndicator", "RoundButton", "ToolButton", "ToolSeparator", "ContextMenu", "TreeView",
    "ListView", "TableView", "Tray", "Chart", "Scene3D", "Surface", "MediaPlayer", "WebView", "RichText", "CodeEditor",
    "Shortcut", "Calendar"].map((tg) => ({ key: tg, el: el(tg, "") })),
  { key: "MenuBar", el: el("MenuBar", `<Menu title="F"><MenuItem onClick={() => setS(1)}>i</MenuItem></Menu>`) },
  { key: "TabBar", el: el("TabBar", "<TabButton>t</TabButton>") },
  { key: "TabButton", el: `<TabBar>${el("TabButton", "t")}</TabBar>` },
  // <Menu> needs trigger= (self-managed) or ref= (imperative); the two forms exclude each other.
  { key: "Menu", el: el("Menu", `<MenuItem onClick={() => setS(1)}>i</MenuItem>`),
    base: (a) => /^(ref|trigger)=/.test(a) ? a : `trigger={<button>t</button>} ${a}` },
  { key: "MenuItem", el: `<Menu trigger={<button>t</button>}>${el("MenuItem", "i")}</Menu>` },
  { key: "MenuSeparator", el: `<Menu trigger={<button>t</button>}><MenuItem onClick={() => setS(1)}>i</MenuItem>${el("MenuSeparator", "")}</Menu>` },
  { key: "Show", el: el("Show", "<text>x</text>") }, { key: "For", el: el("For", "{(r) => <text>{r}</text>}") },
  { key: "Index", el: el("Index", "{(r) => <text>{r()}</text>}") },
  { key: "VirtualList", el: el("VirtualList", "{(r) => <text>{r}</text>}") },
  { key: "Switch", el: el("Switch", "<Match when={s() > 0}><text>m</text></Match>") },
  { key: "Match", el: `<Switch>${el("Match", "<text>m</text>")}</Switch>` },
  { key: "Dynamic", el: el("Dynamic") }, { key: "Suspense", el: el("Suspense", "<text>x</text>") },
];

/** Attribute names that are NOT in a tag's contract get tried too: they must be reported. */
const FOREIGN = `id style key href target hidden autofocus lang spellcheck tabIndex role aria-label aria-hidden data-x
  onDblClick onMouseEnter onMouseLeave onMouseDown onWheel onFocus onBlur onKeyUp onSubmit onScroll onClick onInput
  onChange value placeholder disabled title open name type min max step checked src alt size edge current label
  icon text html data columns keyed`.split(/\s+/);

/** The value written for an attribute under test: a sentinel that must surface in the QML. */
function sample(spec: AttrSpec | undefined, name: string): string {
  const kind = typeof spec === "string" ? spec : spec?.kind ?? "expr";
  switch (kind) {
    case "fn": case "handler": return `{() => __pk()}`;
    case "object": return `{{ __pk: s() > 0 }}`;
    case "elem": return name === "trigger" ? `{<button>__pk</button>}` : `{<text>__pk</text>}`;
    case "flag": return "";
    case "lit": return typeof spec === "string" ? `"__pk"` : `"${(spec as { values: readonly string[] }).values[0]}"`;
    default: return `{__pk}`;
  }
}

// Emitters that reject a declared attribute with their own, more specific message.
const EMITTER_REJECTS = new Set(["input:date min", "input:date max"]);

test("contract covers every native registry tag and every builtin tag", () => {
  for (const name of nativeTags.keys()) assert.ok(name in ELEMENT_ATTRS, `no attribute contract for registry tag <${name}>`);
  for (const name of BUILTIN_TAGS) assert.ok(name in ELEMENT_ATTRS, `no attribute contract for builtin <${name}>`);
  const covered = new Set(FIXTURES.map((f) => f.key));
  for (const key of Object.keys(ELEMENT_ATTRS)) if (key !== "Window") assert.ok(covered.has(key), `no sweep fixture for <${key}>`);
});

test("sweep: every tag × attribute is emitted or reported, never silently ignored", async () => {
  const problems: string[] = [];
  for (const fx of FIXTURES) {
    const spec = ELEMENT_ATTRS[fx.key];
    const at = (a: string) => fx.el.replace("{A}", fx.base ? fx.base(a) : a);
    const base = await emit(at(""));
    if (base.err && !(fx.key === "Menu")) { problems.push(`<${fx.key}> fixture does not emit: ${base.err}`); continue; }
    const names = new Set([...Object.keys(spec), ...FOREIGN]);
    for (const name of names) {
      const s = spec[name];
      const v = sample(s, name);
      // An attribute read only together with another one gets it (the lone form must be rejected).
      const needs = s !== undefined ? REQUIRES[name] : undefined;
      if (needs) {
        const alone = await emit(at(`${name}=${v}`));
        if (!alone.err) problems.push(`<${fx.key}> ${name} without ${needs}: silently ignored`);
      }
      const attr = (needs ? `${needs} ` : "") + (v ? `${name}=${v}` : name);
      const r = await emit(at(attr));
      const where = `<${fx.el.split(/[ >]/)[0].slice(1)}…${fx.key}> ${attr}`;
      if (s === undefined) {
        // Not in the contract → must be REPORTED.
        if (r.err) continue;
        if (r.warned && isWarnOnlyAttr(name)) continue;
        problems.push(`${where}: silently ignored (not in the contract, no error)`);
        continue;
      }
      if (EMITTER_REJECTS.has(`${fx.key} ${name}`)) { if (!r.err) problems.push(`${where}: expected the emitter's rejection`); continue; }
      if (r.err) { problems.push(`${where}: declared but throws: ${r.err}`); continue; }
      const kind = typeof s === "string" ? s : s.kind;
      if (kind === "flag" || (kind === "lit" && typeof s !== "string")) {
        if (kind === "lit") {
          // An enumerated literal: an unknown value is rejected.
          const bad = await emit(at(`${name}="__pk"`));
          if (!bad.err) problems.push(`${where}: enum attribute accepts an unknown value silently`);
        } else if (r.out === base.out) problems.push(`${where}: declared flag has no effect on the output`);
        continue;
      }
      if (!r.out!.includes("__pk")) problems.push(`${where}: declared but its value never reaches the QML`);
      // A dynamic value where only a literal is supported must be rejected, not dropped.
      if (kind === "lit") {
        const dyn = await emit(at(`${name}={__pk}`));
        if (!dyn.err) problems.push(`${where}: a dynamic value is silently accepted for a literal-only attribute`);
      }
    }
  }
  assert.deepEqual(problems, []);
});

test("warn tier: a web-only attribute warns (once) instead of failing; anything else fails", async () => {
  const w = await emit(`<div aria-label="x" data-k="1" role="button" tabIndex={0}><text>t</text></div>`);
  assert.ok(!w.err && w.warned, w.err ?? "no warning");
  const e = await emit(`<div id="x"><text>t</text></div>`);
  assert.match(e.err ?? "", /<div> does not support `id` natively/);
  const sp = await emit(`<div {...mm}><text>t</text></div>`);
  assert.match(sp.err ?? "", /spread attributes/);
  const fnOnly = await emit(`<textarea onInput={setS} />`);
  assert.match(fnOnly.err ?? "", /onInput must be an inline function/);
});

test("<Window>: its four attributes are emitted; anything else is reported", async () => {
  const { generate } = await import("../src/index.ts");
  const ok = await generate(`export function App(){ return <Window title={"T" + 1} width={300} height={200} visible><div /></Window>; }`, "w.tsx");
  assert.match(ok.entry, /title: "T" \+ 1/);
  await assert.rejects(generate(`export function App(){ return <Window title="T" icon="x.png" visible><div /></Window>; }`, "w.tsx"), /<Window> does not support `icon`/);
});

test("<input type> outside the supported set is reported (it used to become a text field)", async () => {
  for (const ty of ["file", "color", "hidden", "time", "submit"]) {
    const r = await emit(`<input type="${ty}" />`);
    assert.match(r.err ?? "", new RegExp(`<input type="${ty}"> is not supported natively`), ty);
  }
  assert.match((await emit(`<input type={"te" + "xt"} />`)).err ?? "", /type must be a string literal/);
});
