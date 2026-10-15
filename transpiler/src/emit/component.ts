import * as t from "@babel/types";
import _generate from "@babel/generator";
import _traverse from "@babel/traverse";
import { emitQml } from "./qml.ts";
import { emitExpr, emitPattern, shadowOwnParams, type Scope } from "./expr.ts";
import { emitStmt, emitValue, emitBindingStmt, emitStatements } from "./stmt.ts";
import { onDepsCall, type EffectInfo } from "../model/symbols.ts";
import { BATCH_MODULE_PATH } from "./runtime.ts";
import { analyzeChildrenAccessors } from "../model/symbols.ts";
import { analyzeSignals, analyzeProps, analyzeSetup, analyzeResources, analyzeMutableLocals, analyzeRenderLocals, renderLocalLiveTaint, analyzeHelpers, analyzeModuleFunctions, moduleLevelMutableNames, collectModuleFunctionClosure, collectFetcherDeps, effectDeps, type HelperInfo, type ProviderInfo, type UseContextBinding } from "../model/symbols.ts";
import { safeName, memberName, changedHandlerName } from "../names/safe.ts";
import { hParts, isHCall, isFragmentTag } from "../ast/h.ts";

const generate: typeof _generate = (_generate as any).default ?? _generate;
const traverse: typeof _traverse = (_traverse as any).default ?? _traverse;

const INDENT = "    ";

/** Context wiring threaded into a component's emit: the `useContext` bindings it owns (consumer), the
 *  per-component-tag context roles (for instance injection), and the global value-shape table. */
export interface CtxWiring {
  /** This component's own `const c = useContext(Ctx)` bindings (consumer). */
  useContext?: UseContextBinding[];
  /** Per used component-tag context role (provides/consumes), for emitInstance. */
  componentMeta?: Map<string, { provides?: string; consumes?: string[] }>;
  /** ctx → value member shape, shared across the module graph. */
  ctxValueShape?: Record<string, Record<string, "accessor" | "fn">>;
}

/**
 * Emit a component as its own QML type: the render tree, with the root decorated by `id: __self`
 * and one reactive `property` per signal (initial value via emitExpr). The signal properties are
 * the bare names the render's bindings read.
 */
export interface ComponentOptions {
  /** The app calls `batch` somewhere: every computation defers itself while a batch is open. */
  batching?: boolean;
  /** Per instantiated component (use-name → declared props), for spread props `<C {...o}>`. */
  componentProps?: Map<string, Set<string>>;
  /** Props this component must declare beyond the ones it reads: forwarded by a `{...props}` spread. */
  forwardedProps?: Set<string>;
}

export function emitComponentType(fn: t.Function, render: t.CallExpression, components?: Map<string, string>, contexts?: Set<string>, provider?: ProviderInfo | null, ctx?: CtxWiring, moduleFile?: t.File, jsImports?: Record<string, string>, moduleInit?: string[], foreignQml?: Set<string>, foreignImports?: Map<string, string>, options: ComponentOptions = {}): string[] {
  // Solid's API as imported by this module (local name → exported name), so a call to an API the
  // emitter does not model is a compile error rather than a call to an undefined function.
  const solidApi = moduleFile ? solidImports(moduleFile) : undefined;
  // Runtime helpers the emitted code asked for (untrack → __untrack, batch → SolidBatch).
  const runtime = { untrack: false, batch: false };
  const table = analyzeSignals(fn);
  const props = analyzeProps(fn);
  const propAliases = new Set(props.aliases);
  const resources = analyzeResources(fn);
  const mutableLocalsMap = analyzeMutableLocals(fn);
  const mutableLocals = mutableLocalsMap.size > 0 ? new Set(mutableLocalsMap.keys()) : undefined;
  // Computed BEFORE analyzeRenderLocals (moved up from its old position below) — it needs both
  // `table` and `helpersMap` to seed candidates from every eagerly-evaluated position, not just
  // `render` itself (see analyzeRenderLocals' and eagerBindingSeeds' own doc comments).
  const helpersMap = analyzeHelpers(fn);
  const renderLocalsMap = analyzeRenderLocals(fn, render, mutableLocals, table, helpersMap);
  // `provider.valueLocal` (a context provider's own value object, e.g. `const counter = { count,
  // increment }`) is a DELIBERATE exception: it is EXPECTED to stay a live QML binding (every
  // descendant's `counter.count()` must track the signal — that's the whole point of a context
  // value). Remove it BEFORE the live-taint check below, which exists for every OTHER plain
  // const/let (those must be FROZEN once constructed, matching Solid's "evaluate once" semantics).
  if (provider?.valueLocal) renderLocalsMap.delete(provider.valueLocal);
  // A render-local is "live-tainted" when its initializer reads a signal/memo/resource, or a prop
  // (bare `props`, or a mergeProps/splitProps alias) — anything this transpiler represents as a
  // bare, live QML property read. Those get frozen once in Component.onCompleted (below) instead
  // of staying a `readonly property var` binding forever (renderLocalLiveTaint's doc comment).
  const isLiveName = (name: string): boolean => {
    const sym = table.get(name);
    if (sym && (sym.kind === "signal" || sym.kind === "memo" || sym.kind === "resource")) return true;
    if (props.param && name === props.param) return true;
    return propAliases.has(name);
  };
  const renderLocalsLive = renderLocalLiveTaint(renderLocalsMap, isLiveName);
  const renderLocals = renderLocalsMap.size > 0 ? new Set(renderLocalsMap.keys()) : undefined;
  const childrenAccessors = analyzeChildrenAccessors(fn, render, props.param, propAliases);
  validateChildrenSlot(render, props.param, propAliases, childrenAccessors);
  const collectedRefs: string[] = [];
  // Consumer wiring: local → ctx, so `c.member` resolves to `__ctx_<ctx>.member` in render bindings.
  const ctxBindings: Record<string, string> = {};
  for (const b of ctx?.useContext ?? []) ctxBindings[b.local] = b.ctx;
  const hasCtxBindings = Object.keys(ctxBindings).length > 0;
  const inputCounter = { n: 0 };
  const hoverCounter = { n: 0 };
  // Mutable flag set by widget emitters during the render pass.
  // `flag`: any Templates widget emitted → prepend the import.
  // `calendar`: MonthGrid / DayOfWeekRow used → upgrade to 6.3 (AbstractMonthGrid added in 6.3).
  const usedWidgets: { flag: boolean; calendar: boolean; popupWindow: boolean; extraImports?: Set<string>; widgetLib?: boolean } =
    { flag: false, calendar: false, popupWindow: false };
  // Radio button group names collected by emitRadioButton; each unique name becomes one
  // T.ButtonGroup { id: __group_<name> } child of the root item (emitted into lifecycle below).
  const buttonGroups = new Map<string, string[]>();
  // Module-level HELPER FUNCTIONS — `function f(){}` / `const f = (...) => ...` at module scope,
  // NOT a component — available to EVERY component in the file that references them, in every
  // position (bindings/text/handlers/memos/other helpers…), same as a local helper (analyzeHelpers
  // above). Selected transitively: a referenced helper may call another module helper or read a
  // module const (collectModuleFunctionClosure walks that closure) — so only what's actually
  // reachable from THIS component is emitted into it (no dead helpers). A name already bound
  // locally in this component (signal/render-local/mutable-local/own helper/prop) SHADOWS the
  // module one — real JS lexical scoping — so it is never pulled in under that name.
  const directRefs = collectIdentifierNames(fn);
  const moduleFunctionsAll = moduleFile ? analyzeModuleFunctions(moduleFile) : new Map<string, HelperInfo>();
  let moduleFnDecls = new Map<string, HelperInfo>();
  // Module-level `const NAME = …` referenced by this component — directly, OR through a module
  // helper it calls, OR through another module const's initializer — surfaced as `__const_NAME`
  // properties (QML property names cannot start with an upper-case letter, and these usually do;
  // the identifier is aliased in scope). A pure-literal data table and any other initializer
  // (`new Map()`, a RegExp, a call, an async function lowered by Babel) alike: it is evaluated once
  // per component instance. Helpers and consts are closed over together (a const may call a helper,
  // a helper may read a const), to a fixpoint.
  const moduleConstDecls = new Map<string, t.Expression>();
  if (moduleFile) {
    const moduleMutableNames = moduleLevelMutableNames(moduleFile);
    const decls = moduleLevelDecls(moduleFile);
    const locallyBound = new Set<string>([...table.keys(), ...renderLocalsMap.keys(), ...mutableLocalsMap.keys(), ...helpersMap.keys(), ...props.used]);
    const fetchers = new Set(resources.map((r) => r.fetcher)); // inlined by the resource loader
    const wanted = new Set([...directRefs].filter((n) => !locallyBound.has(n)));
    for (let changed = true; changed;) {
      changed = false;
      const closure = collectModuleFunctionClosure([...wanted].filter((n) => moduleFunctionsAll.has(n)), moduleFunctionsAll, moduleMutableNames);
      moduleFnDecls = closure.selected;
      for (const n of closure.referencedNames) if (!wanted.has(n)) { wanted.add(n); changed = true; }
      for (const [name, d] of decls) {
        if (!wanted.has(name) || moduleConstDecls.has(name) || fetchers.has(name) || moduleFunctionsAll.has(name)) continue;
        if (d.kind === "skip") continue;
        if (d.kind === "error") throw new Error(d.message);
        moduleConstDecls.set(name, d.init);
        for (const n of identifiersIn(d.init)) if (!wanted.has(n)) { wanted.add(n); changed = true; }
      }
    }
    checkModuleConstMutation(fn, [...moduleFnDecls.values()], new Set(moduleConstDecls.keys()));
  }
  const helpers = (helpersMap.size > 0 || moduleFnDecls.size > 0)
    ? new Set([...helpersMap.keys(), ...moduleFnDecls.keys()])
    : undefined;
  const constAliases: Record<string, string> = {};
  for (const name of moduleConstDecls.keys()) constAliases[name] = `__const_${safeName(name)}`;
  // A module helper whose name starts upper-case is a QML method `__uc_Name` (memberName): its
  // callers — this component AND the other module helpers — reach it through this alias.
  const moduleFnAliases: Record<string, string> = {};
  for (const name of moduleFnDecls.keys()) if (memberName(name) !== safeName(name)) moduleFnAliases[name] = memberName(name);
  // Same for the component-body names promoted to members that a bare identifier reaches (a
  // render-local const, a mutable let, a local helper — signals/memos/stores/props resolve through
  // cellRef, which applies memberName itself). `const SIZE_STEPS = […]` → `__uc_SIZE_STEPS`.
  const memberAliases: Record<string, string> = { ...moduleFnAliases };
  for (const name of [...renderLocalsMap.keys(), ...mutableLocalsMap.keys(), ...helpersMap.keys()])
    if (memberName(name) !== safeName(name)) memberAliases[name] = memberName(name);

  // Pre-collect ref variables (`ref={x}`) across the render tree. Refs are pushed to
  // collectedRefs during emit (for the id line), but render-tree HANDLERS (a <Shortcut>/menu
  // onActivated calling `x.method()`) are emitted before the ref element and need the
  // substitution up front — otherwise `x` emits bare and QML throws "x is not defined".
  const refAliases: Record<string, string> = {};
  {
    const walk = (node: t.Node | null | undefined): void => {
      if (!node || typeof node !== "object") return;
      if (t.isObjectExpression(node)) {
        for (const pr of node.properties) {
          if (t.isObjectProperty(pr) && !pr.computed && t.isIdentifier(pr.key, { name: "ref" }) && t.isIdentifier(pr.value))
            refAliases[pr.value.name] = `_ref_${safeName(pr.value.name)}`;
        }
      }
      for (const key of Object.keys(node)) {
        const v = (node as any)[key];
        if (Array.isArray(v)) v.forEach(walk);
        else if (v && typeof v === "object" && typeof v.type === "string") walk(v);
      }
    };
    walk(render);
  }
  // Module-const aliases + member aliases + ref aliases apply in EVERY expression position this
  // component emits — not just the render tree. ONE place owns the combined map so no Scope built
  // below can forget it.
  const baseLocals: Record<string, string> | undefined =
    (moduleConstDecls.size > 0 || Object.keys(refAliases).length > 0 || Object.keys(memberAliases).length > 0)
      ? { ...constAliases, ...memberAliases, ...refAliases } : undefined;
  const lazyRefs = new Set<string>();
  const scope: Scope = {
    table, mode: "binding", propsParam: props.param ?? undefined, propAliases, components, contexts, refs: collectedRefs, lazyRefs, foreignQml, foreignImports,
    inputCounter, hoverCounter, usedWidgets, buttonGroups, resources: resources.map((r) => r.name), jsImports, solidApi, runtime,
    ...(childrenAccessors.size ? { childrenAccessors } : {}),
    ...(moduleFile && referencesComponentAsValue(fn, moduleComponentNames(moduleFile, components)) ? { componentsAsValues: true } : {}),
    ...(Object.keys(props.rests).length ? { restExcludes: props.rests } : {}),
    ...(options.componentProps ? { componentProps: options.componentProps } : {}),
    ...(baseLocals ? { locals: baseLocals } : {}),
    ...(mutableLocals ? { mutableLocals } : {}),
    ...(helpers ? { helpers } : {}),
    ...(hasCtxBindings ? { ctxBindings, ctxValueShape: ctx?.ctxValueShape } : {}),
    ...(ctx?.componentMeta ? { componentMeta: ctx.componentMeta, providerStack: {}, ctxProvCounter: { n: 0 } } : {}),
  };
  // A root-level fragment needs a QML host: wrap in a primitive-less CssRect (a plain
  // layout box the engine treats as a transparent container).
  let renderRoot = render;
  {
    const { tag } = hParts(render);
    if (isFragmentTag(tag)) {
      renderRoot = t.callExpression(t.identifier("h"), [
        t.stringLiteral("div"),
        t.nullLiteral(),
        ...hParts(render).children.filter((c): c is t.Expression => t.isExpression(c)),
      ]);
    }
  }
  const lines = emitQml(renderRoot, scope, 0);

  // If any widget was emitted, prepend the Templates import so T.* types resolve.
  // Pin the MINIMUM revision the component actually needs (owner directive): 6.0 base,
  // 6.3 for the calendar Abstract* templates, 6.8 only when a native-window dropdown
  // (Popup.popupType) is present — so simple forms keep the widest Qt compatibility.
  if (usedWidgets.flag) {
    const ver = usedWidgets.popupWindow ? "6.8" : usedWidgets.calendar ? "6.3" : "6.0";
    lines.unshift("", `import QtQuick.Templates ${ver} as T`);
  }
  // Widget-library components (Button, …) live in the solidqml.Widgets module (the loader adds the
  // app root to the import path). The transpiler instantiates them instead of hand-emitting internals.
  if (usedWidgets.widgetLib) lines.unshift("", `import solidqml.Widgets 1.0 as W`);
  // Extra QML imports requested by native-tag emitters (Qt.labs.platform, QtQuick.Dialogs, …).
  if (usedWidgets.extraImports?.size) {
    lines.unshift("", ...[...usedWidgets.extraImports].sort());
  }

  const openIdx = lines.findIndex((l) => /\{\s*$/.test(l));
  // The root may already carry an id from a ref={x} (e.g. id: _ref_myDiv). A QML object can have only
  // one id, so if the root already has one we don't add a separate `id: __self`.
  let rootHasId = false;
  if (openIdx >= 0) {
    for (let i = openIdx + 1; i < lines.length; i++) {
      if (/^\s*\S.*\{\s*$/.test(lines[i]) || /^\s*\}/.test(lines[i])) break; // child opened / root closed
      if (/^\s*id:\s*[A-Za-z_]\w*/.test(lines[i])) { rootHasId = true; break; }
    }
  }
  const decls: string[] = rootHasId ? [] : [`${INDENT}id: __self`];
  // Refs inside a lazily mounted <Show>: the element publishes itself here while it is mounted.
  for (const name of lazyRefs) decls.push(`${INDENT}property var ${name}: null`);
  // Prop reads inside reactive initializers (e.g. createSignal(props.x || 0)) resolve through the
  // props param to the sibling prop property. (init mode is non-self-qualified → bare property ref.)
  const initScope: Scope = { table, mode: "init", propsParam: props.param ?? undefined, propAliases, jsImports, solidApi, runtime, ...(baseLocals ? { locals: baseLocals } : {}) };
  // Props read here, plus props this component forwards with a `{...props}` spread (index.ts).
  // A root that is itself a component instance (`return <Mid …/>`) IS that component's object: its
  // props are already properties here. A prop of this component with the same name is the same
  // property — forwarded as is (`label={props.label}`, `{...props}`) it needs no declaration and no
  // binding (`label: label` would bind the property to itself); a default becomes a plain value
  // the parent can override. Any other use of the name would need two properties in one: fail loud.
  const rootTag = hParts(render).tag;
  const rootName = t.isIdentifier(rootTag) ? rootTag.name : "";
  const rootInner = rootName && components?.has(rootName) ? options.componentProps?.get(rootName) : undefined;
  const inherited = new Set<string>();
  if (rootInner) {
    const rootLine = (k: string) => new RegExp(`^${INDENT}${memberName(k).replace(/\$/g, "\\$")}: `);
    for (const name of new Set([...props.used, ...(options.forwardedProps ?? [])])) {
      if (name === "children" || !rootInner.has(name)) continue;
      const at = lines.findIndex((l, k) => k > openIdx && rootLine(name).test(l));
      if (at >= 0 && lines[at] !== `${INDENT}${memberName(name)}: ${memberName(name)}`)
        throw new Error(`<${rootName}> is this component's root and already has a prop \`${name}\`: the component cannot also take its own \`${name}\` and pass something else — rename one, or wrap the root in a <div>`);
      if (at >= 0) lines.splice(at, 1);
      inherited.add(name);
    }
    for (const sym of table.values())
      if ("name" in sym && rootInner.has(sym.name))
        throw new Error(`<${rootName}> is this component's root and already has a prop \`${sym.name}\`: a ${sym.kind} of the same name would be the same property — rename it, or wrap the root in a <div>`);
  }
  for (const name of new Set([...props.used, ...(options.forwardedProps ?? [])])) {
    if (name === "children") continue;
    // A signal/memo of the same name already owns this property (the prop binds to it); skip the
    // duplicate prop declaration so the type stays valid QML.
    const sameName = table.get(name);
    if (sameName && (sameName.kind === "signal" || sameName.kind === "memo")) continue;
    const def = props.defaults[name];
    if (inherited.has(name)) { if (def) decls.push(`${INDENT}${memberName(name)}: ${emitExpr(def, initScope)}`); continue; }
    decls.push(def
      ? `${INDENT}property var ${memberName(name)}: ${emitExpr(def, initScope)}`
      : `${INDENT}property var ${memberName(name)}`);
  }
  for (const sym of table.values()) {
    if (sym.kind === "signal") {
      const init = resolveSignalInit(sym.name, sym.init, props.param ?? null, initScope);
      decls.push(`${INDENT}property var ${memberName(sym.name)}: ${init}`);
    } else if (sym.kind === "store") {
      // A store is a reactive `property var` holding its array/object; setters reassign it to rebind.
      const init = sym.init ? emitExpr(sym.init, initScope) : "undefined";
      decls.push(`${INDENT}property var ${memberName(sym.name)}: ${init}`);
    } else if (sym.kind === "memo") {
      // A memo is a QML binding: the engine tracks every read. Same name resolution as the render
      // (props, helpers, context members, mutable locals…).
      const memoScope: Scope = { ...scope, mode: "binding" };
      if (sym.fn) {
        // prev / initial value / on(deps, fn): the memo keeps its own state in a plain JS object
        // (never reassigned, so reading it is not a dependency). With on(), only `deps` is read
        // tracked; the function runs untracked (__untrack) with (input, prevInput, prevValue).
        const st = `__${safeName(sym.name)}_state`;
        const init = sym.init ? emitExpr(sym.init, initScope) : "undefined";
        decls.push(`${INDENT}readonly property var ${st}: ({ prev: ${init}${sym.on ? `, input: undefined, deferred: ${sym.on.defer}` : ""} })`);
        let body: string;
        if (sym.on) {
          runtime.untrack = true;
          body = `var __s = ${st}; var __in = ${emitExpr(onDepsCall(sym.on.deps), memoScope)}; ` +
            `if (__s.deferred) { __s.deferred = false; return __s.prev; } ` +
            `__s.prev = __untrack(function() { return (${emitValue(sym.fn, memoScope)})(__in, __s.input, __s.prev); }); __s.input = __in; return __s.prev;`;
        } else {
          body = `var __s = ${st}; __s.prev = (${emitValue(sym.fn, memoScope)})(__s.prev); return __s.prev;`;
        }
        decls.push(`${INDENT}readonly property var ${memberName(sym.name)}: (function() { ${body} })()`);
      } else if (t.isBlockStatement(sym.body)) {
        // Block-bodied memo: emit as an IIFE so the property binding reads reactive cells BARE.
        // `const visible = createMemo(() => { const f = filter(); return items.filter(...) })`
        // →  `readonly property var visible: (function() { var f = filter; return items.filter(...) })()`
        const body = sym.body.body.map((s) => emitBindingStmt(s, memoScope)).join(" ");
        decls.push(`${INDENT}readonly property var ${memberName(sym.name)}: (function() { ${body} })()`);
      } else {
        decls.push(`${INDENT}readonly property var ${memberName(sym.name)}: ${emitExpr(sym.body, memoScope)}`);
      }
    }
  }
  // Immutable locals read by render bindings must be visible as QML object properties. A
  // live-tainted one (renderLocalsLive) is emitted WITHOUT `readonly` — it still gets the right
  // value from this same binding at construction, but it also needs to be FROZEN once afterward
  // (the `name = name` self-assignment below, in Component.onCompleted), which a readonly property
  // cannot accept at all.
  for (const [name, init] of renderLocalsMap) {
    const kind = renderLocalsLive.has(name) ? "property" : "readonly property";
    decls.push(`${INDENT}${kind} var ${memberName(name)}: ${emitExpr(init, initScope)}`);
  }
  // Module-const data tables (collected above; aliased in scope as __const_NAME).
  for (const [name, init] of moduleConstDecls)
    decls.push(`${INDENT}readonly property var ${constAliases[name]}: ${emitExpr(init, initScope)}`);
  // Mutable locals (`let x = <literal>` mutated in handlers): emit as `property var x: <init>` so
  // handler closures share a persistent slot on the instance rather than a local var in onCompleted.
  for (const [name, init] of mutableLocalsMap) {
    decls.push(`${INDENT}property var ${memberName(name)}: ${emitExpr(init, initScope)}`);
  }
  // A resource owns three properties: the value (`r`), a loading flag (`r_loading`, starts true so
  // the Suspense fallback shows before the first fetch settles), and an error slot (`r_error`). A
  // hidden `__load_<r>` property holds the (re)loader closure so both onCompleted and the source
  // Connections can invoke the same loader.
  for (const r of resources) {
    const rn = memberName(r.name);
    decls.push(`${INDENT}property var ${rn}`);
    decls.push(`${INDENT}property bool ${rn}_loading: true`);
    decls.push(`${INDENT}property var ${rn}_error`);
    decls.push(`${INDENT}property var __load_${rn}`);
  }
  // A consumer declares one `property var __ctx_<ctx>` per consumed context — the slot the parent
  // injects the providing instance into. Member access resolves through it (`__ctx_<ctx>.member`).
  for (const ctxName of new Set(Object.values(ctxBindings)))
    decls.push(`${INDENT}property var __ctx_${safeName(ctxName)}`);

  // A context provider exposes each non-accessor value member as a function property, self-qualified
  // (the function mutates the provider's own signals). Accessor members are already signal/memo
  // properties above, so the provider INSTANCE carries the whole context value.
  if (provider) {
    const fnScope: Scope = { table, mode: "handler", propsParam: props.param ?? undefined, propAliases, components, jsImports, ...(baseLocals ? { locals: baseLocals } : {}), ...(mutableLocals ? { mutableLocals } : {}) };
    for (const [name, node] of Object.entries(provider.fnMembers))
      decls.push(`${INDENT}property var ${memberName(name)}: ${emitValue(node, fnScope)}`);
  }
  // Local helper functions: `function f(params) { body }` and param/block-bodied `const f = ...` →
  // emitted as QML object methods. Signal reads and mutable-local reads are BARE (QML object scope
  // resolves them); calls to other helpers are also bare sibling method calls.
  if (helpersMap.size > 0) {
    const helperScope: Scope = {
      table, mode: "handler", propsParam: props.param ?? undefined, propAliases, components, jsImports,
      ...(baseLocals ? { locals: baseLocals } : {}),
      ...(mutableLocals ? { mutableLocals } : {}),
      ...(helpers ? { helpers } : {}),
    };
    for (const [name, info] of helpersMap) decls.push(`${INDENT}${emitHelperMethod(name, info, helperScope)}`);
  }
  // Module-level helper functions selected above (moduleFnDecls): emitted as QML object methods
  // too, but with a DELIBERATELY MINIMAL scope — unlike a local helper, a module-level function is
  // NOT lexically inside this component (it's defined at module top level, and the SAME body is
  // copied, unmodified, into every OTHER component that also calls it). It must not see this
  // component's own signals/props/mutable-locals/refs: giving it `table`/`mutableLocals` etc. would
  // let it accidentally resolve a bare name against THIS component's unrelated local state instead
  // of failing the way real JS would. The only things in its real lexical scope are: its own
  // params (shadowed below, same rule as a local helper) and module-level consts (constAliases —
  // the same literal alias string regardless of which component embeds it, so sharing this
  // component's `constAliases` map is exactly the set it needs, already narrowed to what's used).
  if (moduleFnDecls.size > 0) {
    const moduleFnScope: Scope = {
      table: new Map(), mode: "handler", jsImports,
      ...(Object.keys(constAliases).length || Object.keys(moduleFnAliases).length ? { locals: { ...constAliases, ...moduleFnAliases } } : {}),
    };
    for (const [name, info] of moduleFnDecls) decls.push(`${INDENT}${emitHelperMethod(name, info, moduleFnScope)}`);
  }
  // init(self) sidecar (inline): non-render setup statements + onMount bodies run in
  // Component.onCompleted on __self; onCleanup(fn) registrations collect into __cleanups and run in
  // Component.onDestruction. Imperative context → handler mode (signal reads bare, setters assign).
  const { setup, effectInfos, afterMount } = analyzeSetup(fn, mutableLocals, renderLocals);
  // Map each ref variable (let myDiv) to its QML id so that onMount body references resolve
  // correctly in emitStmt/emitExpr (checked via scope.locals before the symbol table).
  // collectedRefs entries are plain var names; the id is always _ref_<varName> as emitted by emitQml.
  const refLocals: Record<string, string> = {};
  const refVarNames = new Set<string>();
  for (const refName of collectedRefs) {
    refLocals[refName] = `_ref_${safeName(refName)}`;
    refVarNames.add(refName);
  }
  // Filter out bare ref variable declarations (e.g. `let myDiv`) from setup — they are not
  // imperative statements to emit; the element id already serves as the binding target.
  const filteredSetup = setup.filter((s) => {
    if (!t.isVariableDeclaration(s)) return true;
    // Drop if ALL declarators are bare ref vars (no init, or undefined init) used as refs, OR the
    // provider's value-object local const (structural — its members are exposed as properties).
    return !s.declarations.every((d) =>
      t.isIdentifier(d.id) && (
        (refVarNames.has(d.id.name) && (!d.init || (t.isIdentifier(d.init) && d.init.name === "undefined"))) ||
        (provider?.valueLocal === d.id.name)
      )
    );
  });
  const setupScope: Scope = {
    table, mode: "handler", propsParam: props.param ?? undefined, propAliases, components,
    locals: { ...(baseLocals ?? {}), ...refLocals }, jsImports, solidApi, runtime,
    ...(mutableLocals ? { mutableLocals } : {}),
    ...(helpers ? { helpers } : {}),
    ...(hasCtxBindings ? { ctxBindings, ctxValueShape: ctx?.ctxValueShape } : {}),
  };
  // A live-tainted render-local (renderLocalsLive) was emitted above as a non-readonly `property
  // var` bound to its initializer — correct at construction, but still LIVE until frozen. Freeze
  // each one here, in DECLARATION order (renderLocalsMap's own iteration order), so a later
  // tainted local that reads an earlier one sees it already frozen — not that the ORDER changes
  // the frozen VALUE (nothing can mutate the signal/prop between these synchronous statements),
  // but it keeps the mechanism legible. Self-assignment (`x = x`) clears the binding (verified
  // against the real engine) and must run before any user onMount/effect code, which is why these
  // statements come first, right after the hoisted module-level ones.
  const freezeRenderLocals = [...renderLocalsMap.keys()]
    .filter((name) => renderLocalsLive.has(name))
    .map((name) => `${memberName(name)} = ${memberName(name)};`);

  // Computations (createEffect / createRenderEffect / createComputed): each is ONE method on the
  // root, `__effectN()`, run once at its Solid time and re-run by a `Connections` handler per
  // reactive dependency — see emitComputation. A render effect / computed runs at its position in
  // setup; createEffect and onMount run after setup, in source order (Solid queues both as effects).
  const effectNames = effectInfos.map((_, k) => `__effect${k}`);
  const firstRun = (name: string) => `__started.${name} = true; ${name}();`;
  const stmtEffect = new Map<t.Statement, number>(effectInfos.map((e, k) => [e.stmt, k]));
  const onCompletedBody: string[] = [
    // Hoisted module-level statements (already emitted in their own module's import scope):
    // import-time code runs before any component setup.
    ...(moduleInit ?? []),
    ...freezeRenderLocals,
    ...filteredSetup.map((s) => stmtEffect.has(s) ? firstRun(effectNames[stmtEffect.get(s)!]) : emitStmt(s, setupScope)),
    // Each onMount body in its own block: block-scoped declarations of two bodies never collide.
    ...afterMount.map((m) => "effect" in m ? firstRun(effectNames[m.effect])
      : t.isBlockStatement(m.mount) ? `{ ${emitStatements(m.mount.body, setupScope)} }` : `${emitExpr(m.mount, setupScope)};`),
  ];
  // createResource: inline the module-level fetcher (+ its async-helper closure), then build a
  // (re)loader closure stored on __self. It sets _loading true, runs Promise.resolve(fetcher(source))
  // and writes the value/_loading/_error back. The loader runs once in onCompleted and re-runs via a
  // Connections handler on the source signal. Self-qualified everywhere (detached closures).
  const conns: string[] = [];
  if (resources.length) {
    if (!moduleFile) throw new Error("emitComponentType: createResource requires the module AST to inline the fetcher");
    // Inline each distinct fetcher's module-level decls once (deduped by node), in source order.
    const emittedDecl = new Set<t.Node>();
    const inlineDecls: string[] = [];
    for (const r of resources) {
      for (const node of collectFetcherDeps(moduleFile, r.fetcher)) {
        if (emittedDecl.has(node)) continue;
        emittedDecl.add(node);
        inlineDecls.push(generate(node, { compact: false, concise: true }).code);
      }
    }
    const loaderStmts: string[] = [...inlineDecls];
    for (const r of resources) {
      const rn = memberName(r.name);
      const srcSym = table.get(r.source);
      const srcRef = memberName(r.source); // the source signal — bare property read
      loaderStmts.push(
        `__load_${rn} = function() { ${rn}_loading = true; ${rn}_error = undefined; ` +
          `Promise.resolve(${safeName(r.fetcher)}(${srcRef})).then(function(v) { ${rn} = v; ${rn}_loading = false; })` +
          `.catch(function(e) { ${rn}_error = e; ${rn}_loading = false; }); };`,
        `__load_${rn}();`,
      );
      // Reload when the source signal changes (only if the source is a tracked reactive cell).
      if (srcSym && (srcSym.kind === "signal" || srcSym.kind === "memo")) {
        conns.push(
          `${INDENT}Connections {`,
          `${INDENT}${INDENT}target: __self`,
          `${INDENT}${INDENT}function ${changedHandlerName(srcRef)}() { __load_${rn}(); }`,
          `${INDENT}}`,
        );
      }
    }
    onCompletedBody.push(loaderStmts.join(" "));
  }
  // Names that are known NOT to be a QObject/context reference, so a bare `x.y` read inside an
  // effect is never mistaken for one (office.spellReady IS one; a call to an imported helper or a
  // module-level data table never is). See effectDeps' own doc comment for the full resolution
  // order; this set only feeds its LAST-resort "unknown global" bucket.
  const effectKnownNonReactive = new Set<string>([
    ...(helpers ?? []),
    ...Object.keys(jsImports ?? {}),
    ...moduleConstDecls.keys(),
    ...(contexts ?? []),
    ...(components?.keys() ?? []),
    ...(foreignQml ?? []),
    ...(mutableLocals ?? []),
    // Every other name declared at the component's top level (a `let` array mutated in place, a
    // plain object): a JS value, never a QObject — signals/memos/stores/refs/props are resolved
    // before this last-resort bucket, so listing them here too is harmless.
    ...componentTopLevelNames(fn),
  ]);
  const rootId = rootHasId ? rootIdOf(lines, openIdx) : "__self";
  const disposeEffects: string[] = [];
  const depRuns = new Map<string, { dep: { target: string; prop: string }; runs: string[] }>();
  effectInfos.forEach((info, k) => {
    const name = effectNames[k];
    // The dependencies: what the body reads — or, with on(deps, fn), exactly what `deps` reads.
    const depSource: t.Node = info.on ? onDepsCall(info.on.deps) : info.fn.body;
    const deps = effectDeps(depSource, table, {
      propsParam: props.param,
      propAliases,
      ctxBindings: hasCtxBindings ? ctxBindings : undefined,
      ctxValueShape: ctx?.ctxValueShape,
      refLocals,
      knownNonReactive: effectKnownNonReactive,
      helpers: helpersMap,
    });
    const c = emitComputation(name, info, setupScope, rootId, !!options.batching);
    decls.push(...c.decls.map((d) => `${INDENT}${d}`));
    if (c.cleanups) disposeEffects.unshift(c.cleanups); // Solid disposes the last-created first
    for (const dep of deps) {
      const key = `${dep.target}\u0000${dep.prop}`;
      const g = depRuns.get(key) ?? depRuns.set(key, { dep, runs: [] }).get(key)!;
      g.runs.push(name);
    }
  });
  // One Connections per dependency, re-running its computations in CREATION order (Solid's order;
  // separate Connections would fire in reverse — QML notifies the newest endpoint first).
  for (const { dep, runs } of depRuns.values()) {
    conns.push(
      `${INDENT}Connections {`,
      `${INDENT}${INDENT}target: ${dep.target}`,
      `${INDENT}${INDENT}function ${changedHandlerName(dep.prop)}() { ${runs.map((r) => `${r}();`).join(" ")} }`,
      `${INDENT}}`,
    );
  }
  // A computation only re-runs once it has run: a dependency that changes earlier (a prop set while
  // the component is built, a computed writing a signal during setup) must not run it before its
  // turn — Solid queues it and runs it once. `__started` records the first run.
  if (effectInfos.length) decls.push(`${INDENT}readonly property var __started: ({})`);
  const needsBatchModule = runtime.batch || (!!options.batching && effectInfos.length > 0);
  const lifecycle: string[] = [];
  // One T.ButtonGroup per unique radio `name`; attached property on each T.RadioButton wires exclusivity.
  for (const [gname, ids] of buttonGroups) {
    // `order` carries the radios in DECLARATION order — ButtonGroup.buttons follows
    // attachment order, which incubation scrambles (arrows walked Free→Team→Pro).
    lifecycle.push(`${INDENT}T.ButtonGroup { id: __group_${safeName(gname)}; readonly property var order: [${ids.join(", ")}] }`);
  }
  if (onCompletedBody.length) {
    lifecycle.push(`${INDENT}property var __cleanups: []`);
    lifecycle.push(`${INDENT}Component.onCompleted: { ${onCompletedBody.join(" ")} }`);
    // Unmount, in Solid's order: each computation's cleanups (last-created computation first), then
    // the component's own onCleanup registrations — every list newest-first.
    lifecycle.push(`${INDENT}Component.onDestruction: { ${disposeEffects.join(" ")}${disposeEffects.length ? " " : ""}for (var i = __cleanups.length - 1; i >= 0; i--) __cleanups[i](); }`);
  }
  // untrack(fn) inside a QML binding: the read must not register a dependency. A binding records
  // every property read made while it evaluates; a SIGNAL HANDLER does not (Qt suspends property
  // capture for handler expressions — qqmljavascriptexpression.cpp), so the untracked function runs
  // inside a handler of a private signal, synchronously, and hands its value back through a closure.
  if (runtime.untrack) {
    decls.push(`${INDENT}function __untrack(f) { var r = {}; __untracker.run(function() { r.v = f(); }); return r.v; }`);
    lifecycle.push(`${INDENT}QtObject { id: __untracker; signal run(var f); onRun: (f) => f() }`);
  }
  lifecycle.push(...conns);

  if (openIdx >= 0) lines.splice(openIdx + 1, 0, ...decls, ...lifecycle);
  if (needsBatchModule) lines.unshift(BATCH_IMPORT);
  return lines;
}

const BATCH_IMPORT = `import "${BATCH_MODULE_PATH}" as SolidBatch`;

/** One computation as a root method `name()`:
 *   - runs the previous run's onCleanup registrations first (newest first), when the body has any;
 *   - with `batch` in the app, defers itself while a batch is open (SolidBatch.defer) — the flush
 *     calls `<name>_run` once per computation, after the batch;
 *   - calls the user function with its previous value when it takes one (`fn(prev)`, first run
 *     with createEffect's second argument), keeping the return value for the next run — as in
 *     Solid, a returned function is that value, not a cleanup;
 *   - `on(deps, fn, { defer })`: reads `deps` (the ONLY dependencies), skips the first run when
 *     deferred, and calls `fn(input, prevInput, prevValue)`.
 *  Returns the declarations (state properties + method(s)) and the unmount statement for its
 *  cleanups (null when it has none). */
function emitComputation(name: string, info: EffectInfo, setupScope: Scope, rootId: string, batching: boolean): { decls: string[]; cleanups: string | null } {
  const decls: string[] = [];
  const hasCleanup = containsCall(info.fn, "onCleanup");
  const list = `${name}_cleanups`;
  const scope: Scope = hasCleanup ? { ...setupScope, cleanupList: list } : setupScope;
  const nParams = info.fn.params.length;
  const body: string[] = [];
  if (hasCleanup) {
    decls.push(`property var ${list}: []`);
    body.push(`var __c = ${list}; ${list} = []; for (var __i = __c.length - 1; __i >= 0; __i--) __c[__i]();`);
  }
  const wantsPrev = info.on ? nParams >= 3 : nParams >= 1;
  if (wantsPrev) decls.push(`property var ${name}_prev: ${info.init ? emitExpr(info.init, setupScope) : "undefined"}`);
  if (info.on) {
    if (info.on.defer) decls.push(`property bool ${name}_deferred: true`);
    if (nParams >= 2) decls.push(`property var ${name}_input`);
    body.push(`var __in = ${emitExpr(onDepsCall(info.on.deps), scope)};`);
    if (info.on.defer) body.push(`if (${name}_deferred) { ${name}_deferred = false; return; }`);
    const args = ["__in", ...(nParams >= 2 ? [`${name}_input`] : []), ...(nParams >= 3 ? [`${name}_prev`] : [])];
    const call = `(${emitValue(info.fn, scope)})(${args.join(", ")})`;
    body.push(wantsPrev ? `${name}_prev = ${call};` : `${call};`);
    if (nParams >= 2) body.push(`${name}_input = __in;`);
  } else if (wantsPrev) {
    body.push(`${name}_prev = (${emitValue(info.fn, scope)})(${name}_prev);`);
  } else {
    body.push(t.isBlockStatement(info.fn.body) ? emitStatements(info.fn.body.body, scope) : `${emitValue(info.fn.body, scope)};`);
  }
  const started = `if (!__started.${name}) return;`;
  if (batching) {
    decls.push(`function ${name}() { ${started} if (!SolidBatch.defer(${rootId}, "${name}_run")) ${name}_run(); }`);
    decls.push(`function ${name}_run() { ${body.join(" ")} }`);
  } else {
    decls.push(`function ${name}() { ${started} ${body.join(" ")} }`);
  }
  return { decls, cleanups: hasCleanup ? `for (var __i = ${list}.length - 1; __i >= 0; __i--) ${list}[__i]();` : null };
}

/** Does `node` contain a call to `name(...)` (anywhere, including nested callbacks)? */
function containsCall(node: t.Node, name: string): boolean {
  let found = false;
  const walk = (n: unknown): void => {
    if (found || !n || typeof n !== "object") return;
    if (Array.isArray(n)) { for (const x of n) walk(x); return; }
    const nn = n as t.Node;
    if (!nn.type) return;
    if (t.isCallExpression(nn) && t.isIdentifier(nn.callee, { name })) { found = true; return; }
    for (const key of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[key]);
  };
  walk(node);
  return found;
}

/** The root object's existing id (a `ref` on the render root) — the root carries only one id. */
function rootIdOf(lines: string[], openIdx: number): string {
  for (let i = openIdx + 1; i < lines.length; i++) {
    const m = /^\s*id:\s*([A-Za-z_]\w*)/.exec(lines[i]);
    if (m) return m[1];
  }
  return "__self";
}

/** A helper (local or module-level function) as a QML object method. Its own params shadow any
 *  same-named const alias / mutable local inherited from `scope` — real JS lexical scoping (e.g.
 *  `function f(RULE_FIELDS) { … }` must NOT rewrite its own param); a destructured param keeps its
 *  pattern. safeName so a reserved param (e.g. `id`→`id_`) matches its body refs. */
function emitHelperMethod(name: string, info: HelperInfo, scope: Scope): string {
  const paramStr = info.paramNodes.map((p) => emitPattern(p, scope)).join(", ");
  const bodyScope = info.params.length
    ? { ...scope, locals: { ...(scope.locals ?? {}), ...Object.fromEntries(info.params.map((p) => [p, safeName(p)])) } }
    : scope;
  return `function ${memberName(name)}(${paramStr}) { ${emitStatements(info.body.body, bodyScope)} }`;
}

/** Resolve the initial value for a signal property, handling the same-name prop+signal collision.
 *
 *  When a signal `S` has the same name as a prop (the prop seeds the signal's initial value), the
 *  naive init `props.S || 0` would emit `property var S: S || 0` — a self-referential binding loop.
 *
 *  Decision (Option 1): fold the prop operand out; the parent seeds the property via override
 *  (e.g. `CounterProvider { count: 5 }`), and the init becomes the fallback default:
 *  - `createSignal(props.S || default)` → `property var S: default`
 *  - `createSignal(props.S ?? default)` → `property var S: default`
 *  - `createSignal(props.S)` (exact prop ref)    → `property var S: undefined`
 *  - any other shape referencing `props.S`        → throw (scope guard)
 */
function resolveSignalInit(
  signalName: string,
  initNode: t.Expression | null,
  propsParam: string | null,
  initScope: Scope,
): string {
  if (!initNode) return "undefined";
  // Detect whether this init references props.<signalName> — only if there is a props param.
  const isPropRef = (n: t.Node): boolean =>
    !!propsParam && t.isMemberExpression(n) && !n.computed &&
    t.isIdentifier(n.object, { name: propsParam }) && t.isIdentifier(n.property, { name: signalName });

  // Deep-walk: does the init reference props.<signalName> ANYWHERE (not just at the top)? A nested
  // ref like `props.S + 1` must still be caught — otherwise it would emit a self-referential
  // `property var S: S + 1` binding loop.
  let hasPropRef = false;
  const walk = (n: t.Node): void => {
    if (hasPropRef) return;
    if (isPropRef(n)) { hasPropRef = true; return; }
    for (const key of Object.keys(n)) {
      const v = (n as unknown as Record<string, unknown>)[key];
      if (Array.isArray(v)) { for (const c of v) if (c && typeof (c as t.Node).type === "string") walk(c as t.Node); }
      else if (v && typeof (v as t.Node).type === "string") walk(v as t.Node);
    }
  };
  walk(initNode);
  if (!hasPropRef) return emitExpr(initNode, initScope); // no same-name prop reference — normal path

  // Same-name prop reference detected — only the two folds below are supported.
  // Case 1: EXACTLY props.<signalName>  →  default undefined (parent seeds)
  if (isPropRef(initNode)) return "undefined";

  // Case 2: props.<signalName> || default  or  props.<signalName> ?? default  →  emit default only
  if (t.isLogicalExpression(initNode) && (initNode.operator === "||" || initNode.operator === "??")
      && isPropRef(initNode.left)) {
    return emitExpr(initNode.right, initScope);
  }

  // Any other shape — scope guard
  throw new Error(
    `signal "${signalName}" seeded from same-named prop supports only props.${signalName} or props.${signalName} || default`,
  );
}

/** {props.children} is supported only as a direct child of the root render element (it mounts via
 *  the type's inherited default property). Anywhere deeper needs an explicit slot alias — a later
 *  plan — so reject it rather than silently mount children at the wrong place.
 *  NB: only the props param is checked, not mergeProps/splitProps aliases (alias.children) — no
 *  current example mixes child slots with prop helpers; tighten when one does. */
function validateChildrenSlot(render: t.CallExpression, propsParam: string | null, propAliases: Set<string>, accessors: Set<string>): void {
  if (!propsParam) return;
  const isMarker = (n: t.Node): boolean => (t.isMemberExpression(n) && !n.computed && t.isIdentifier(n.object)
    && (n.object.name === propsParam || propAliases.has(n.object.name)) && t.isIdentifier(n.property, { name: "children" }))
    || (t.isCallExpression(n) && n.arguments.length === 0 && t.isIdentifier(n.callee) && accessors.has(n.callee.name));
  const rootChildren = new Set(hParts(render).children);
  const walk = (n: t.Node): void => {
    if (isHCall(n)) {
      const { tag, props, children } = hParts(n);
      for (const a of [tag, ...(props ? [props] : []), ...children]) {
        if (isMarker(a)) { if (!rootChildren.has(a)) throw new Error("props.children outside the root render element is not supported in this plan"); }
        else if (t.isExpression(a)) walk(a);
      }
    }
  };
  walk(render);
}

/** Names that denote components in this module: its own component functions, the components it
 *  instantiates, and capitalised imports from relative modules. */
function moduleComponentNames(file: t.File, used?: Map<string, string>): Set<string> {
  const out = new Set<string>(used?.keys() ?? []);
  const returnsH = (body: t.Node | null | undefined) => !!body && t.isBlockStatement(body) && body.body.some((st) => t.isReturnStatement(st) && isHCall(st.argument));
  for (const node of file.program.body) {
    const decl = t.isExportNamedDeclaration(node) || t.isExportDefaultDeclaration(node) ? node.declaration : node;
    if (t.isFunctionDeclaration(decl) && decl.id && returnsH(decl.body)) out.add(decl.id.name);
    if (t.isImportDeclaration(node) && node.source.value.startsWith("."))
      for (const sp of node.specifiers) if (/^[A-Z]/.test(sp.local.name)) out.add(sp.local.name);
  }
  return out;
}

/** Does the component use a component name as a value (not as an h() tag)? */
function referencesComponentAsValue(fn: t.Function, components: Set<string>): boolean {
  let found = false;
  const walk = (n: unknown, parent: t.Node | null): void => {
    if (found || !n || typeof n !== "object") return;
    if (Array.isArray(n)) { for (const x of n) walk(x, parent); return; }
    const nn = n as t.Node;
    if (!nn.type) return;
    if (t.isIdentifier(nn) && components.has(nn.name)) {
      const isTag = !!parent && isHCall(parent) && parent.arguments[0] === nn;
      const isKey = !!parent && ((t.isMemberExpression(parent) && parent.property === nn && !parent.computed)
        || (t.isObjectProperty(parent) && parent.key === nn && !parent.computed));
      if (!isTag && !isKey) { found = true; return; }
    }
    for (const k of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[k], nn);
  };
  walk(fn.body, null);
  return found;
}

/** The names this module imports from Solid (`solid-js`, `solid-js/store`, `solid-js/web`):
 *  local name → exported name. */
function solidImports(file: t.File): Map<string, string> {
  const out = new Map<string, string>();
  for (const node of file.program.body) {
    if (!t.isImportDeclaration(node) || !/^solid-js(\/(store|web))?$/.test(node.source.value)) continue;
    for (const sp of node.specifiers)
      if (t.isImportSpecifier(sp)) out.set(sp.local.name, t.isIdentifier(sp.imported) ? sp.imported.name : sp.imported.value);
  }
  return out;
}

type ModuleDecl = { kind: "const"; init: t.Expression } | { kind: "skip" } | { kind: "error"; message: string };

const SOLID_PRIMITIVES = new Set(["createSignal", "createStore", "createMemo", "createEffect", "createResource",
  "createRenderEffect", "createComputed", "createSelector", "createRoot", "createMutable", "createDeferred", "onMount", "onCleanup"]);

/** Every top-level variable of a module (plain or exported) and what a component referencing it
 *  gets: a const to emit, a binding owned elsewhere (`createContext`, a function — module helpers),
 *  or a compile error for what has no per-component equivalent (module-level reactive state, JSX,
 *  `let`/`var` state shared between components). */
function moduleLevelDecls(file: t.File): Map<string, ModuleDecl> {
  const out = new Map<string, ModuleDecl>();
  const solid = solidImports(file);
  for (const node of file.program.body) {
    const st = t.isExportNamedDeclaration(node) && node.declaration ? node.declaration : node;
    if (!t.isVariableDeclaration(st)) continue;
    for (const d of st.declarations) {
      const names = Object.keys(t.getBindingIdentifiers(d.id));
      const init = d.init;
      const api = init && t.isCallExpression(init) && t.isIdentifier(init.callee) ? solid.get(init.callee.name) : undefined;
      const set = (v: ModuleDecl) => { for (const n of names) out.set(n, v); };
      if (api === "createContext") { set({ kind: "skip" }); continue; }
      if (api && SOLID_PRIMITIVES.has(api)) {
        set({ kind: "error", message: `module-level ${api}(…) (${names.join(", ")}): reactive state created outside a component is not supported natively — create it in a component and share it through a context provider or props` });
        continue;
      }
      if (st.kind !== "const") {
        set({ kind: "error", message: `module-level \`${st.kind} ${names.join(", ")}\`: module state is not shared between components natively (each component gets its own copy) — make it a const, move it into the component, or into a plain .ts module imported here (one shared JS module)` });
        continue;
      }
      if (!t.isIdentifier(d.id) || !init) {
        set({ kind: "error", message: `module-level destructuring \`const ${names.join(", ")}\` is not supported natively — declare each constant` });
        continue;
      }
      if (t.isArrowFunctionExpression(init) || t.isFunctionExpression(init)) { set({ kind: "skip" }); continue; } // a module helper
      if (containsH(init)) {
        set({ kind: "error", message: `module-level JSX constant \`${d.id.name}\` is not supported natively — render it from a component` });
        continue;
      }
      set({ kind: "const", init });
    }
  }
  return out;
}

function containsH(n: t.Node): boolean {
  let found = false;
  const walk = (x: unknown): void => {
    if (found || !x || typeof x !== "object") return;
    if (Array.isArray(x)) { for (const y of x) walk(y); return; }
    const nn = x as t.Node;
    if (!nn.type) return;
    if (isHCall(nn)) { found = true; return; }
    for (const k of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[k]);
  };
  walk(n);
  return found;
}

/** Identifier names referenced in an expression (property keys excluded). */
function identifiersIn(n: t.Node): Set<string> {
  const out = new Set<string>();
  const walk = (x: unknown, parent: t.Node | null): void => {
    if (!x || typeof x !== "object") return;
    if (Array.isArray(x)) { for (const y of x) walk(y, parent); return; }
    const nn = x as t.Node;
    if (!nn.type) return;
    if (t.isIdentifier(nn)) {
      const isKey = !!parent && ((t.isMemberExpression(parent) && parent.property === nn && !parent.computed)
        || (t.isObjectProperty(parent) && parent.key === nn && !parent.computed));
      if (!isKey) out.add(nn.name);
    }
    for (const k of t.VISITOR_KEYS[nn.type] ?? []) walk((nn as unknown as Record<string, unknown>)[k], nn);
  };
  walk(n, null);
  return out;
}

const MUTATORS = new Set(["push", "pop", "shift", "unshift", "splice", "sort", "reverse", "fill", "copyWithin", "set", "delete", "clear", "add"]);

/** A module const is instantiated once per component instance (a property), so mutating it is
 *  not shared the way a JS module's object is: fail loud where the component (or a module helper it
 *  uses) mutates one in place. */
function checkModuleConstMutation(fn: t.Function, helpers: HelperInfo[], consts: Set<string>): void {
  if (!consts.size) return;
  const rootOf = (e: t.Node): t.Identifier | null => {
    let n = e;
    while (t.isMemberExpression(n) || t.isOptionalMemberExpression(n)) n = n.object;
    return t.isIdentifier(n) ? n : null;
  };
  const check = (body: t.BlockStatement | t.Expression, params: t.Node[]) => {
    const program = t.isBlockStatement(body) ? t.program(body.body) : t.program([t.expressionStatement(body)]);
    const own = new Set(params.flatMap((p) => Object.keys(t.getBindingIdentifiers(p))));
    const isModule = (id: t.Identifier, path: { scope: { getBinding(n: string): unknown } }) =>
      consts.has(id.name) && !own.has(id.name) && !path.scope.getBinding(id.name);
    const fail = (name: string, how: string) => {
      throw new Error(`module-level const ${name} is mutated (${how}) — each component instance gets its own copy, so the change is not shared as in JS: move ${name} into the component, or into a plain .ts module imported here (one shared JS module)`);
    };
    traverse(t.file(program), {
      AssignmentExpression(path) {
        const l = path.node.left;
        if (!t.isMemberExpression(l)) return;
        const r = rootOf(l);
        if (r && isModule(r, path)) fail(r.name, "an assignment to a member");
      },
      UpdateExpression(path) {
        const r = t.isMemberExpression(path.node.argument) ? rootOf(path.node.argument) : null;
        if (r && isModule(r, path)) fail(r.name, `${path.node.operator} on a member`);
      },
      UnaryExpression(path) {
        if (path.node.operator !== "delete" || !t.isMemberExpression(path.node.argument)) return;
        const r = rootOf(path.node.argument);
        if (r && isModule(r, path)) fail(r.name, "delete");
      },
      CallExpression(path) {
        const c = path.node.callee;
        if (!t.isMemberExpression(c) || c.computed || !t.isIdentifier(c.property) || !MUTATORS.has(c.property.name)) return;
        const r = rootOf(c.object);
        if (r && isModule(r, path)) fail(r.name, `.${c.property.name}(…)`);
      },
    });
  };
  check(fn.body as t.BlockStatement, fn.params);
  for (const h of helpers) check(h.body, h.paramNodes);
}

/** Names declared directly in the component body (variables, incl. destructured, and functions). */
function componentTopLevelNames(fn: t.Function): string[] {
  if (!t.isBlockStatement(fn.body)) return [];
  const out: string[] = [];
  for (const s of fn.body.body) {
    if (t.isVariableDeclaration(s)) {
      for (const d of s.declarations) out.push(...Object.keys(t.getBindingIdentifiers(d.id)));
    } else if (t.isFunctionDeclaration(s) && s.id) {
      out.push(s.id.name);
    }
  }
  return out;
}

/** Every Identifier name appearing in the component function — an over-approximation of what it
 *  references (shadowing ignored; module data tables are distinctive names in practice). */
function collectIdentifierNames(fn: t.Function): Set<string> {
  const names = new Set<string>();
  const walk = (node: unknown): void => {
    if (!node || typeof node !== "object") return;
    if (Array.isArray(node)) { for (const e of node) walk(e); return; }
    const n = node as { type?: string; name?: string };
    if (n.type === "Identifier" && n.name) names.add(n.name);
    for (const key of Object.keys(n)) {
      if (key === "loc" || key === "leadingComments" || key === "trailingComments" || key === "innerComments") continue;
      walk((n as Record<string, unknown>)[key]);
    }
  };
  walk(fn.params);
  walk(fn.body);
  return names;
}
