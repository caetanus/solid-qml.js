// Regenerates the code-derived tables in docs/support-matrix.md (release-plan M9).
//
//   node --import tsx scripts/support-matrix.mjs          rewrite the generated regions in place
//   node --import tsx scripts/support-matrix.mjs --check  exit 1 if the doc is out of date
//
// Every table between `<!-- generated:NAME:begin -->` and `<!-- generated:NAME:end -->` comes from
// the code, not from memory:
//   - elements: the transpiler's attribute contract (transpiler/src/emit/attrs.ts ELEMENT_ATTRS) plus
//     the QML types each tag actually emits (each tag is run through generate() and the emitted
//     types are read back from the output);
//   - widgets: the C++ types registered into solidqml.Widgets (src/widgets/solidwidgets.cpp) and the
//     opt-in .qml modules (qml/solidqml/Widgets/*/qmldir);
//   - probes: small Solid/JSX programs run through generate(); the "transpiler today" column is the
//     real verdict (accepted, or the exact compile error). The status/notes columns are written by
//     hand next to each probe — when the transpiler's verdict changes, --check fails and whoever
//     changed it has to revisit the status.
// transpiler/test/support-matrix-doc.test.ts runs --check.
import { readFile, writeFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { generate } from "../transpiler/src/index.ts";
import { ELEMENT_ATTRS } from "../transpiler/src/emit/attrs.ts";
import { TEXT_TAGS } from "../transpiler/src/emit/qml.ts";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const DOC = path.join(ROOT, "docs/support-matrix.md");

const HEAD = `import { createSignal, createMemo, createEffect, createResource, untrack, batch, on, createComputed,
  createRenderEffect, createSelector, createRoot, onMount, onCleanup, Show, For, Index, Switch, Match, Suspense,
  ErrorBoundary, lazy, createContext, useContext, mergeProps, splitProps, children } from "solid-js";
import { createStore, produce, reconcile, createMutable } from "solid-js/store";
import { Dynamic, Portal } from "solid-js/web";
`;

/** Run one program through the real transpiler; returns all emitted QML, or the compile error. */
async function run(body, files = {}) {
  const ow = console.warn;
  const warnings = [];
  console.warn = (...a) => warnings.push(a.join(" "));
  try {
    const r = await generate(HEAD + body, "/virtual/app.tsx", { readFile: async (p) => {
      if (p in files) return files[p];
      throw new Error(`no file ${p}`);
    } });
    return { qml: [r.entry, ...Object.values(r.components)].join("\n"), warnings };
  } catch (e) {
    return { err: String(e.message).split("\n")[0], warnings };
  } finally {
    console.warn = ow;
  }
}

const cell = (s) => String(s).replace(/\|/g, "\\|").replace(/\n/g, " ");

// ── elements ────────────────────────────────────────────────────────────────────────────────────
const el = (tag, kids = "x") => kids ? `<${tag}>${kids}</${tag.split(" ")[0]}>` : `<${tag} />`;
/** contract key → [tags shown, a valid snippet]. Parent-consumed keys are placed inside their parent. */
const ELEMENT_FIXTURES = {
  box: ["`div`, `section`, `nav`, `li`, `ul`, `label`, `table`… (any other lowercase tag)", el("div")],
  text: [[...TEXT_TAGS].map((t) => `\`${t}\``).join(", "), el("text")],
  button: ["`button`", el("button")],
  img: ["`img`", `<img src="a.png" />`],
  icon: ["`icon`", `<icon name="edit-find-symbolic" />`],
  "input:text": ["`input` (type text/password/email/search/tel/url)", el("input", "")],
  "input:checkbox": ["`input type=checkbox`", el(`input type="checkbox"`, "")],
  "input:radio": ["`input type=radio`", el(`input type="radio"`, "")],
  "input:range": ["`input type=range`", el(`input type="range"`, "")],
  "input:number": ["`input type=number`", el(`input type="number"`, "")],
  "input:date": ["`input type=date`", el(`input type="date"`, "")],
  textarea: ["`textarea`", el("textarea", "")],
  select: ["`select`", el("select", `<option value="a">A</option>`)],
  option: ["`option`", null],
  progress: ["`progress`", el("progress", "")],
  fieldset: ["`fieldset`", el("fieldset")],
  legend: ["`legend`", null],
  dialog: ["`dialog`", el("dialog")],
  details: ["`details`", el("details", "<summary>s</summary><text>b</text>")],
  summary: ["`summary`", null],
  StackView: ["`StackView`", el("StackView", "<div>a</div><div>b</div>")],
  "MenuBar>Menu": ["`Menu` inside `MenuBar`", null],
  MenuBar: ["`MenuBar`", el("MenuBar", `<Menu title="F"><MenuItem onClick={() => setS(1)}>i</MenuItem></Menu>`)],
  TabBar: ["`TabBar`", el("TabBar", "<TabButton>t</TabButton>")],
  TabButton: ["`TabButton`", null],
  Menu: ["`Menu`", `<Menu trigger={<button>t</button>}><MenuItem onClick={() => setS(1)}>i</MenuItem></Menu>`],
  MenuItem: ["`MenuItem`", null],
  MenuSeparator: ["`MenuSeparator`", null],
  Show: ["`Show`", el("Show when={s() > 0}", "<text>x</text>")],
  For: ["`For`", el("For each={[1, 2]}", "{(r) => <text>{r}</text>}")],
  Index: ["`Index`", el("Index each={[1, 2]}", "{(r) => <text>{r()}</text>}")],
  VirtualList: ["`VirtualList`", el("VirtualList each={[1, 2]}", "{(r) => <text>{r}</text>}")],
  Switch: ["`Switch`", el("Switch", "<Match when={s() > 0}><text>m</text></Match>")],
  Match: ["`Match`", null],
  Dynamic: ["`Dynamic`", `<Dynamic component={s() > 0 ? "section" : "div"}>x</Dynamic>`],
  Suspense: ["`Suspense`", el("Suspense", "<text>x</text>")],
  Window: ["`Window`", null],
};
for (const tg of ["ToolBar", "Swipe", "SplitView", "Drawer", "SwipeView", "PageIndicator", "RangeSlider", "Dial", "Tumbler",
  "DelayButton", "BusyIndicator", "RoundButton", "ToolButton", "ToolSeparator", "ContextMenu", "TreeView", "ListView",
  "TableView", "Tray", "Chart", "Scene3D", "Surface", "MediaPlayer", "WebView", "RichText", "DocumentEditor", "CodeEditor",
  "Shortcut", "Calendar"])
  ELEMENT_FIXTURES[tg] ??= [`\`${tg}\``, el(tg, "")];

const CONSUMED_BY = { option: "select", legend: "fieldset", summary: "details", "MenuBar>Menu": "MenuBar",
  TabButton: "TabBar", MenuItem: "Menu", MenuSeparator: "Menu", Match: "Switch", Window: "(app root)" };

/** The QML types emitted inside the wrapper <div> (the wrapper itself is the first W.Div). */
function emittedTypes(qml) {
  const lines = qml.split("\n");
  const start = lines.findIndex((l) => /^W\.Div \{/.test(l));
  const types = [];
  const imports = new Set();
  for (const l of lines) {
    const m = /^import (solidqml\.Widgets\.\w+)/.exec(l);
    if (m) imports.add(m[1]);
  }
  for (const l of lines.slice(start + 1)) {
    const m = /^\s+([A-Z]\w*(?:\.[A-Z]\w*)?)\s*\{/.exec(l);
    if (!m) continue;
    if (["Binding", "Connections", "Component", "Timer"].includes(m[1])) continue;
    if (m[1] === "Css.CssText" && types.length) continue; // anonymous text runs inside the element
    if (!types.includes(m[1])) types.push(m[1]);
    if (types.length >= 3) break;
  }
  return { types, imports: [...imports] };
}

function kindOf(spec) {
  return typeof spec === "string" ? spec : `${spec.kind}: ${spec.values.join("/")}`;
}

async function elementsTable() {
  const rows = ["| Tag | Emits (QML type) | Attributes (value form) |", "|---|---|---|"];
  for (const [key, spec] of Object.entries(ELEMENT_ATTRS)) {
    const fx = ELEMENT_FIXTURES[key];
    if (!fx) throw new Error(`support-matrix: no fixture for contract key ${key} — add one to ELEMENT_FIXTURES`);
    const [shown, snippet] = fx;
    let emits;
    if (snippet === null) emits = `consumed by \`${CONSUMED_BY[key]}\``;
    else {
      const r = await run(`export function App(){ const [s,setS]=createSignal(0); return <div>${snippet}</div>; }`);
      if (r.err) throw new Error(`support-matrix: fixture for ${key} does not transpile: ${r.err}`);
      const { types, imports } = emittedTypes(r.qml);
      emits = types.map((x) => `\`${x}\``).join(" → ") || "(nothing)";
      if (imports.length) emits += ` (opt-in module \`${imports.join(", ")}\`)`;
    }
    const attrs = Object.entries(spec).map(([n, s]) => `\`${n}\` ${kindOf(s)}`).join(", ") || "none";
    rows.push(`| ${cell(shown)} | ${cell(emits)} | ${cell(attrs)} |`);
  }
  return rows.join("\n");
}

// ── widgets ─────────────────────────────────────────────────────────────────────────────────────
async function widgetsTable() {
  const src = await readFile(path.join(ROOT, "src/widgets/solidwidgets.cpp"), "utf8");
  const names = [...src.matchAll(/qmlRegisterType<\w+>\("solidqml\.Widgets",\s*1,\s*0,\s*"(\w+)"\)/g)].map((m) => m[1]);
  const out = [`**C++ types registered in \`solidqml.Widgets\`** (${names.length}, \`src/widgets/solidwidgets.cpp\`): ` +
    names.map((n) => `\`${n}\``).join(", ") + "."];
  const base = path.join(ROOT, "qml/solidqml/Widgets");
  const mods = [];
  for (const d of (await readdir(base, { withFileTypes: true })).filter((e) => e.isDirectory()).map((e) => e.name).sort()) {
    const qmldir = await readFile(path.join(base, d, "qmldir"), "utf8").catch(() => null);
    if (!qmldir) continue;
    const mod = /^module\s+(\S+)/m.exec(qmldir)?.[1];
    const types = [...qmldir.matchAll(/^(\w+)\s+\d+\.\d+\s+\S+\.qml/gm)].map((m) => m[1]);
    mods.push(`| \`${mod}\` | ${types.map((x) => `\`${x}\``).join(", ")} |`);
  }
  out.push("", "**Opt-in modules** (`.qml`, imported only by an app that uses the tag; `qml/solidqml/Widgets/*/qmldir`):", "",
    "| Module | Types |", "|---|---|", ...mods);
  return out.join("\n");
}

// ── probes ──────────────────────────────────────────────────────────────────────────────────────
const A = (body) => `export function App(){ const [s,setS]=createSignal(0); ${body} }`;
/** [area, construct, program, status, notes/evidence]. Status: ✅ supported · 🟡 partial ·
 *  ❌ rejected (fails loud at compile time) · ⚠️ accepted but WRONG at runtime (do not use). */
const PROBES = [
  ["reactivity", "`createSignal`", A(`return <button onClick={() => setS(s() + 1)}>{s()}</button>;`), "✅", "`transpiler/test/symbols.test.ts`, `examples/counter.tsx`"],
  ["reactivity", "`createMemo`", A(`const d = createMemo(() => s() * 2); return <text>{d()}</text>;`), "✅", "`transpiler/test/derived.test.ts`"],
  ["reactivity", "`createEffect` (deps = signals, props, store fields, context, QObject properties read in the body)", A(`createEffect(() => console.log(s())); return <text>{s()}</text>;`), "✅", "`transpiler/test/effect-reactive-deps.test.ts`; a component's own plain (non-reactive) values are never dependencies"],
  ["reactivity", "`onCleanup` inside an effect", A(`createEffect(() => { const v = s(); onCleanup(() => console.log(v)); }); return <text>{s()}</text>;`), "✅", "the effect's own cleanups run (newest first) before each re-run and on unmount, before the component's — `transpiler/test/reactivity-api.test.ts`; runtime probe in the M2 commit"],
  ["reactivity", "effect returning a cleanup function", A(`createEffect(() => { s(); return () => 1; }); return <text>{s()}</text>;`), "✅", "as in Solid, the return value is the next run's `prev` (`createEffect((prev) => …, init)`), not a cleanup — use `onCleanup`"],
  ["reactivity", "`untrack`", A(`const d = createMemo(() => untrack(s)); return <text>{d()}</text>;`), "✅", "in bindings the read runs inside a signal handler (Qt records no dependency there); in effects it is left out of the dependencies — `transpiler/test/reactivity-api.test.ts`"],
  ["reactivity", "`batch`", A(`return <button onClick={() => batch(() => setS(1))}>x</button>;`), "🟡", "effects (and `createComputed`/`createRenderEffect`) re-run once, after the batch (shared runtime module `modules/solid-batch.mjs`). Bindings and memos still update on each write, so a memo that reads a signal through `untrack` can keep a value from an intermediate write"],
  ["reactivity", "`on(deps, fn)`", A(`createEffect(on(s, (v) => console.log(v))); return <text>{s()}</text>;`), "✅", "only `deps` are dependencies; `fn(input, prevInput, prev)` runs untracked; `{ defer: true }`; also in `createMemo(on(…))` — `transpiler/test/reactivity-api.test.ts`"],
  ["reactivity", "`createComputed` / `createRenderEffect`", A(`createComputed(() => console.log(s())); return <text>{s()}</text>;`), "✅", "same computation as `createEffect`, first run at its position in the component body (before `onMount`/`createEffect`)"],
  ["reactivity", "`createSelector`", A(`const sel = createSelector(s); return <text>{sel(1) ? "a" : "b"}</text>;`), "✅", "`sel(k)` is `k === s()` (or `fn(k, s())`), reactive on the source"],
  ["reactivity", "`createRoot`", A(`createRoot(() => {}); return <text>x</text>;`), "❌", "a component's computations live as long as its QML object; there is no detached owner"],
  ["reactivity", "`createMemo((prev) => …, init)`", A(`const t = createMemo((prev) => prev + s(), 0); return <text>{t()}</text>;`), "✅", "`transpiler/test/reactivity-api.test.ts`"],
  ["reactivity", "a primitive (`createEffect`, `createSignal`, `on`…) called outside a component's top level", A(`onMount(() => { createEffect(() => console.log(s())); }); return <text>x</text>;`), "❌", "declare it at the top level of the component body"],
  ["resources", "`createResource(source, fetcher)`", `async function load(id){ const r = await fetch("https://example.com/" + id); return r.json(); }
    ${A(`const [r] = createResource(s, load); return <Suspense fallback={<text>…</text>}><text>{r() ? r().name : ""}</text></Suspense>;`)}`, "✅", "`examples/fetch.tsx`; value + `loading`/`error`, re-fetch on source change"],
  ["resources", "`createResource` with an inline fetcher", A(`const [r] = createResource(s, async (i) => i); return <text>{r()}</text>;`), "❌", "name the fetcher (a module-level function)"],
  ["stores", "`createStore` + value / `prev => next` / `produce` / `(pred, \"key\", updater)` setters", `export function App(){ const [st, setSt] = createStore([{ id: 1, done: false }]);
    return <div><For each={st}>{(t) => <text>{t.id}</text>}</For><button onClick={() => setSt((t) => t.id === 1, "done", (d) => !d)}>t</button><button onClick={() => setSt(produce((l) => l.push({ id: 2, done: false })))}>a</button></div>; }`, "✅", "`examples/todomvc.tsx`, `transpiler/test/effect-reactive-deps.test.ts`"],
  ["stores", "path setter `set(\"a\", 0, \"b\", v)`", `export function App(){ const [st, setSt] = createStore({ a: [{ b: 1 }] }); return <button onClick={() => setSt("a", 0, "b", 2)}>x</button>; }`, "❌", ""],
  ["stores", "`reconcile`", `export function App(){ const [st, setSt] = createStore({ a: [] }); return <button onClick={() => setSt("a", reconcile([1]))}>x</button>; }`, "❌", ""],
  ["stores", "`createMutable`", `export function App(){ const m = createMutable({ n: 0 }); return <button onClick={() => m.n++}>{m.n}</button>; }`, "❌", "QML bindings cannot observe in-place mutation: use `createStore`"],
  ["control flow", "`<Show when fallback>`", A(`return <Show when={s() > 0} fallback={<text>no</text>}><text>yes</text></Show>;`), "✅", "lazy (mounted only while `when` holds) — `transpiler/test/control.test.ts`, `transpiler/test/silent-drops.test.ts`"],
  ["control flow", "`<Switch>`/`<Match>`", A(`return <Switch fallback={<text>none</text>}><Match when={s() === 1}><text>one</text></Match></Switch>;`), "✅", "lazy; `transpiler/test/control.test.ts`"],
  ["control flow", "`<For>` (reference identity) + index accessor", A(`return <div><For each={[{ n: "a" }]}>{(x, i) => <text>{i()}: {x.n}</text>}</For></div>;`), "✅", "`transpiler/test/for-index-accessor.test.ts`; `meson test for-identity for-identity-virtual`"],
  ["control flow", "`<For key=\"id\">` (reuse rows by a field; native-only extension)", A(`return <div><For each={[{ id: 1 }]} key="id">{(x) => <text>{x.id}</text>}</For></div>;`), "✅", "`transpiler/test/for-key.test.ts`; contract in SOLID-PARITY.md"],
  ["control flow", "`<Index>`", A(`return <div><Index each={["a"]}>{(x) => <text>{x()}</text>}</Index></div>;`), "✅", "`examples/index-list.tsx`"],
  ["control flow", "`<Suspense>`", A(`return <Suspense fallback={<text>…</text>}><text>x</text></Suspense>;`), "✅", "`examples/fetch.tsx`"],
  ["control flow", "`<Dynamic component=\"tag\">` (intrinsic tag)", A(`return <Dynamic component={s() ? "section" : "div"}>x</Dynamic>;`), "✅", "`examples/dynamic.tsx`"],
  ["control flow", "`<Dynamic component={Comp}>` (component value)", `function P(){ return <text>a</text>; } function Q(){ return <text>b</text>; } ${A(`return <Dynamic component={s() ? P : Q} />;`)}`, "✅", "candidates written as a conditional over component names (and tag strings): one lazily mounted branch each, props and children forwarded — `transpiler/test/components-jsx.test.ts`"],
  ["control flow", "`<Dynamic component={c()}>` with a component held in a signal/variable", `function P(){ return <text>a</text>; } ${A(`const [c] = createSignal(P); return <Dynamic component={c()} />;`)}`, "❌", "write the selector as a conditional over component names"],
  ["control flow", "`<Portal>`", A(`return <div><Portal><text>x</text></Portal></div>;`), "❌", ""],
  ["control flow", "`<ErrorBoundary>`", A(`return <ErrorBoundary fallback={<text>e</text>}><text>x</text></ErrorBoundary>;`), "❌", ""],
  ["control flow", "`lazy()`", `const L = lazy(() => import("./x")); export function App(){ return <L />; }`, "❌", "out of scope"],
  ["components", "props, `props.children`, callbacks", `function Card(p){ return <div><text>{p.title}</text>{p.children}</div>; } ${A(`return <Card title="t"><text>x</text></Card>;`)}`, "✅", "`examples/children.tsx`, `transpiler/test/props.test.ts`"],
  ["components", "`mergeProps` / `splitProps` (local part)", `function C(p){ const m = mergeProps({ a: 1 }, p); return <text>{m.a}</text>; } export function App(){ return <C a={2} />; }`, "✅", "`examples/props-helpers.tsx`"],
  ["components", "context: a provider component wrapping `props.children`, `useContext` in descendants", `const Ctx = createContext(); function Prov(p){ const [n, setN] = createSignal(1); return <Ctx.Provider value={{ n }}>{p.children}</Ctx.Provider>; }
    function Child(){ const c = useContext(Ctx); return <text>{c.n()}</text>; } export function App(){ return <Prov><Child /></Prov>; }`, "🟡", "`examples/context.tsx`; nested/multi-instance providers untested"],
  ["components", "context: `<Ctx.Provider>` written inline in the consumer's parent", `const Ctx = createContext(); function Child(){ const c = useContext(Ctx); return <text>{c.n}</text>; } export function App(){ return <Ctx.Provider value={{ n: 1 }}><Child /></Ctx.Provider>; }`, "❌", "use the provider-component form above"],
  ["components", "spread props on a component `<C {...o} />`", `function C(p){ return <text>{p.a}</text>; } export function App(){ const o = { a: 1 }; return <C {...o} />; }`, "✅", "every prop the component declares reads the sources last to first (first defined value wins); `{...props}` forwards — `transpiler/test/components-jsx.test.ts`"],
  ["components", "spread attributes on an element `<div {...o}>`", A(`const o = { class: "a" }; return <div {...o}>x</div>;`), "❌", ""],
  ["components", "`children()` helper", `function C(p){ const c = children(() => p.children); return <div>{c()}</div>; } export function App(){ return <C><text>x</text></C>; }`, "🟡", "`{c()}` in the root element slots the children; inspecting them (`c.toArray()`) fails at compile time"],
  ["components", "`mergeProps(props, { … })` (an object after props overrides)", `function C(p){ const m = mergeProps(p, { a: 1 }); return <text>{m.a}</text>; } export function App(){ return <C a={2} />; }`, "❌", "put the defaults first: `mergeProps({ … }, props)`"],
  ["components", "destructured props `function C({ a })`", `function C({ a }){ return <text>{a}</text>; } export function App(){ return <C a={2} />; }`, "❌", "use `props.a` (destructuring reads each prop once, untracked)"],
  ["components", "`ref={el}` (variable)", A(`let el; onMount(() => console.log(el.width)); return <div ref={el}>x</div>;`), "✅", "`examples/refs.tsx`, `transpiler/test/ref-render.test.ts`"],
  ["components", "`ref={(e) => …}` (callback)", A(`let el; return <div ref={(e) => el = e}>x</div>;`), "❌", ""],
  ["JSX", "inline `style=`", A(`return <div style={{ opacity: 0.5 }}>x</div>;`), "❌", "use a class and CSS"],
  ["JSX", "`classList={{…}}`", A(`return <div classList={{ on: s() > 0 }}>x</div>;`), "✅", "`transpiler/test/qml.test.ts`"],
  ["JSX", "`use:` directives", `function d(el){} export function App(){ return <div use:d>x</div>; }`, "❌", ""],
  ["JSX", "nested fragments", A(`return <div><><text>a</text><text>b</text></></div>;`), "✅", ""],
  ["JSX", "events beyond the contract (`onMouseMove`, `onDblClick`, `onFocus`, `onSubmit`…)", A(`return <div onDblClick={() => setS(1)}>x</div>;`), "❌", "see the element table for each tag's events"],
  ["handlers", "`onClick` with an expression body", A(`return <button onClick={() => setS(s() + 1)}>x</button>;`), "✅", ""],
  ["handlers", "`onClick` with a block body `() => { … }`", A(`return <button onClick={() => { setS(1); }}>x</button>;`), "✅", "the block runs as its own function — `transpiler/test/components-jsx.test.ts`"],
  ["handlers", "`onClick={go}` (named function in the component, `if`/`for…of` inside)", A(`function go(){ if (s() > 2) setS(0); else for (const i of [1, 2]) setS(s() + i); } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/module-fn-positions.test.ts`"],
  ["handlers", "block body in `fn`-kind events (`onInput`, `onChange`, `onKeyDown`…) with `if`", A(`return <input onInput={(e) => { if (e.currentTarget.value) setS(1); else setS(0); }} />;`), "✅", "`examples/widgets.tsx`"],
  ["handlers", "`async` named handler", A(`async function go(){ const r = await fetch("https://example.com"); setS(await r.text()); } return <button onClick={go}>x</button>;`), "❌", "call an async module-level function from an expression-bodied handler, or use `createResource`"],
  ["statements", "`while` / `do…while` in component code", A(`function go(){ let i = 0; while (i < 3) i++; setS(i); } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/statements.test.ts` (emitted methods are executed)"],
  ["statements", "classic `for (;;)` in component code", A(`function go(){ let t = 0; for (let i = 0; i < 3; i++) t += i; setS(t); } return <button onClick={go}>x</button>;`), "✅", "`let`/`const` heads keep per-iteration bindings — `transpiler/test/statements.test.ts`"],
  ["statements", "`switch` in component code", A(`function go(){ switch (s()) { case 0: setS(1); break; default: setS(0); } } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/statements.test.ts`"],
  ["statements", "`try`/`catch` in component code", A(`function go(){ try { setS(JSON.parse("1")); } catch (e) { setS(0); } } return <button onClick={go}>x</button>;`), "✅", "with `finally`, `throw`, labelled `break`/`continue` — `transpiler/test/statements.test.ts`"],
  ["statements", "destructuring (declarations, params, `for…of` heads; defaults; array rest)", A(`function go(){ const [a, b = 2] = [1]; const { c } = { c: 3 }; for (const [k, v] of Object.entries({ x: 1 })) setS(a + b + c + v + k.length); } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/statements.test.ts`"],
  ["statements", "object rest `const { a, ...rest } = o`", A(`function go(){ const { a, ...rest } = { a: 1, b: 2 }; setS(a + rest.b); } return <button onClick={go}>x</button>;`), "❌", "V4 has no object rest: read the fields you need"],
  ["statements", "`class` in component code", A(`function go(){ class K {} setS(new K() ? 1 : 0); } return <button onClick={go}>x</button>;`), "❌", "declare the class in a plain `.ts` module and import it"],
  ["modules", "a helper function imported from a relative non-component module (`import { f } from \"./util\"`)", `import { f } from "./util"; ${A(`return <button onClick={() => setS(f(s()))}>{s()}</button>;`)}`, "✅", "a plain module (no JSX, no Solid import — checked transitively) is mirrored as a JS module next to the QML, one shared instance — `transpiler/test/modules.test.ts`", { "/virtual/util.ts": "export function f(x){ return x + 1; }" }],
  ["modules", "a non-component binding imported from a module that also has JSX or imports solid-js", `import { f } from "./ui"; ${A(`return <button onClick={() => setS(f(s()))}>{s()}</button>;`)}`, "❌", "move the helper into a plain module", { "/virtual/ui.tsx": "export function f(x){ return x; } export function Card(){ return <text>c</text>; }" }],
  ["modules", "module-level reactive state (`const [n, setN] = createSignal(0)` outside a component)", `const [n, setN] = createSignal(0); export function App(){ return <button onClick={() => setN(n() + 1)}>{n()}</button>; }`, "❌", "create it in a component and share it through a context provider or props"],
  ["modules", "module-level `let`/`var` read by a component", `let total = 0; ${A(`return <button onClick={() => setS(total)}>x</button>;`)}`, "❌", "each component would get its own copy: use a const, or a plain `.ts` module (shared)"],
  ["modules", "a module-level const mutated in place (`LIST.push(…)`, `MAP.set(…)`)", `const SEEN = []; ${A(`return <button onClick={() => setS(SEEN.push(1))}>x</button>;`)}`, "❌", "module consts are per component instance: keep shared mutable state in a plain `.ts` module"],
  ["expressions", "RegExp literal in component code", A(`function go(){ setS(String(s()).replace(/0/g, "o")); } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/statements.test.ts`"],
  ["expressions", "RegExp literal in a module-level constant", `const RE = /a+/; ${A(`return <text>{RE.test(String(s())) ? "y" : "n"}</text>;`)}`, "✅", "module consts with any initializer are emitted (one value per component instance) — `transpiler/test/modules.test.ts`"],
  ["expressions", "`new` expressions in component code (`new Date()`, `new Map()`…)", A(`function go(){ setS(new Date().getTime()); } return <button onClick={go}>x</button>;`), "✅", "`transpiler/test/statements.test.ts`"],
  ["expressions", "a module-level constant initialised with `new` (`const M = new Map()`)", `const M = new Map(); ${A(`return <text>{M.size}</text>;`)}`, "✅", "`transpiler/test/modules.test.ts`; mutating it in place fails at compile time (see above)"],
  ["modules", "an `async` module-level function called from a handler", `async function save(x){ await null; return x; } ${A(`return <button onClick={() => save(s())}>x</button>;`)}`, "✅", "Babel lowers it to a const; emitted with its helpers — `transpiler/test/modules.test.ts`"],
  ["expressions", "optional chaining `a?.b`", A(`const [o] = createSignal(null); return <text>{o()?.a}</text>;`), "✅", "`?.`, `?.[k]`, `?.()` — `transpiler/test/statements.test.ts`"],
  ["expressions", "object spread `{ ...a, x: 1 }`", A(`function go(){ const o = { ...{ a: 1 }, b: 2 }; setS(o.a + o.b); } return <button onClick={go}>x</button>;`), "✅", "lowered to `Object.assign({}, …)` (V4 has no object spread) — `transpiler/test/statements.test.ts`"],
  ["expressions", "nullish coalescing `a ?? b`", A(`const [o] = createSignal(null); return <text>{o() ?? "none"}</text>;`), "✅", ""],
  ["expressions", "`BigInt` literal", A(`return <text>{String(10n)}</text>;`), "❌", "V4 has no BigInt"],
];

async function probesTable() {
  const rows = ["| Area | Construct | Status | Transpiler today (generated) | Notes / evidence |", "|---|---|---|---|---|"];
  for (const [area, what, prog, status, notes, files] of PROBES) {
    const r = await run(prog, files);
    const verdict = r.err ? `rejected: \`${cell(r.err).slice(0, 140)}\`` : "accepted";
    const consistent = status === "❌" ? !!r.err : !r.err;
    if (!consistent)
      throw new Error(`support-matrix: probe "${what}" is marked ${status} but the transpiler ${r.err ? "rejects it: " + r.err : "accepts it"} — update its status in scripts/support-matrix.mjs`);
    rows.push(`| ${area} | ${cell(what)} | ${status} | ${verdict} | ${cell(notes)} |`);
  }
  return rows.join("\n");
}

// ── doc splice ──────────────────────────────────────────────────────────────────────────────────
const GENERATORS = { elements: elementsTable, widgets: widgetsTable, probes: probesTable };

export async function render(doc) {
  let out = doc;
  for (const [name, gen] of Object.entries(GENERATORS)) {
    const begin = `<!-- generated:${name}:begin (scripts/support-matrix.mjs — do not edit by hand) -->`;
    const end = `<!-- generated:${name}:end -->`;
    const i = out.indexOf(begin), j = out.indexOf(end);
    if (i < 0 || j < i) throw new Error(`support-matrix: markers for "${name}" missing in docs/support-matrix.md`);
    out = out.slice(0, i + begin.length) + "\n" + (await gen()) + "\n" + out.slice(j);
  }
  return out;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const doc = await readFile(DOC, "utf8");
  const next = await render(doc);
  if (process.argv.includes("--check")) {
    if (next !== doc) {
      console.error("docs/support-matrix.md is out of date — run: node --import tsx scripts/support-matrix.mjs");
      process.exit(1);
    }
    console.log("docs/support-matrix.md is up to date");
  } else {
    await writeFile(DOC, next);
    console.log("docs/support-matrix.md regenerated");
  }
}
