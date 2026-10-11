import * as t from "@babel/types";
import _generate from "@babel/generator";
import { emitQml } from "./qml.ts";
import { emitExpr, shadowOwnParams, type Scope } from "./expr.ts";
import { emitStmt, emitValue, emitBindingStmt } from "./stmt.ts";
import { analyzeSignals, analyzeProps, analyzeSetup, analyzeResources, analyzeMutableLocals, analyzeRenderLocals, renderLocalLiveTaint, analyzeHelpers, analyzeModuleFunctions, moduleLevelMutableNames, collectModuleFunctionClosure, collectFetcherDeps, effectDeps, type HelperInfo, type ProviderInfo, type UseContextBinding } from "../model/symbols.ts";
import { safeName } from "../names/safe.ts";
import { hParts, isHCall, isFragmentTag } from "../ast/h.ts";

const generate: typeof _generate = (_generate as any).default ?? _generate;

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
export function emitComponentType(fn: t.Function, render: t.CallExpression, components?: Map<string, string>, contexts?: Set<string>, provider?: ProviderInfo | null, ctx?: CtxWiring, moduleFile?: t.File, jsImports?: Record<string, string>, moduleInit?: string[], foreignQml?: Set<string>, foreignImports?: Map<string, string>): string[] {
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
  validateChildrenSlot(render, props.param);
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
  let moduleFnReferencedNames = new Set<string>();
  if (moduleFunctionsAll.size > 0) {
    const moduleMutableNames = moduleLevelMutableNames(moduleFile!);
    const locallyBound = new Set<string>([...table.keys(), ...renderLocalsMap.keys(), ...mutableLocalsMap.keys(), ...helpersMap.keys(), ...props.used]);
    const seeds = [...directRefs].filter((n) => moduleFunctionsAll.has(n) && !locallyBound.has(n));
    const closure = collectModuleFunctionClosure(seeds, moduleFunctionsAll, moduleMutableNames);
    moduleFnDecls = closure.selected;
    moduleFnReferencedNames = closure.referencedNames;
  }
  const helpers = (helpersMap.size > 0 || moduleFnDecls.size > 0)
    ? new Set([...helpersMap.keys(), ...moduleFnDecls.keys()])
    : undefined;
  // Module-level `const NAME = <pure literal>` data tables (menus, slides, feeds…) referenced by
  // this component — directly, OR transitively through a module helper this component calls
  // (moduleFnReferencedNames). QML property names cannot start with an upper-case letter (and
  // these usually do), so each one is surfaced as `__const_NAME` and the identifier is aliased
  // in scope.
  const moduleConstDecls = new Map<string, t.Expression>();
  if (moduleFile) {
    const consts = moduleLevelConsts(moduleFile);
    if (consts.size > 0) {
      const referenced = new Set([...directRefs, ...moduleFnReferencedNames]);
      for (const [name, init] of consts) {
        if (!referenced.has(name)) continue;
        if (table.has(name) || renderLocalsMap.has(name) || mutableLocalsMap.has(name) || helpersMap.has(name)) continue;
        moduleConstDecls.set(name, init);
      }
    }
  }
  const constAliases: Record<string, string> = {};
  for (const name of moduleConstDecls.keys()) constAliases[name] = `__const_${safeName(name)}`;

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
  // Module-const aliases + ref aliases apply in EVERY expression position this component emits —
  // not just the render tree. ONE place owns the combined map so no Scope built below can forget it.
  const baseLocals: Record<string, string> | undefined =
    (moduleConstDecls.size > 0 || Object.keys(refAliases).length > 0) ? { ...constAliases, ...refAliases } : undefined;
  const lazyRefs = new Set<string>();
  const scope: Scope = {
    table, mode: "binding", propsParam: props.param ?? undefined, propAliases, components, contexts, refs: collectedRefs, lazyRefs, foreignQml, foreignImports,
    inputCounter, hoverCounter, usedWidgets, buttonGroups, resources: resources.map((r) => r.name), jsImports,
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
  const initScope: Scope = { table, mode: "init", propsParam: props.param ?? undefined, propAliases, jsImports, ...(baseLocals ? { locals: baseLocals } : {}) };
  for (const name of props.used) {
    if (name === "children") continue;
    // A signal/memo of the same name already owns this property (the prop binds to it); skip the
    // duplicate prop declaration so the type stays valid QML.
    const sameName = table.get(name);
    if (sameName && (sameName.kind === "signal" || sameName.kind === "memo")) continue;
    const def = props.defaults[name];
    decls.push(def
      ? `${INDENT}property var ${safeName(name)}: ${emitExpr(def, initScope)}`
      : `${INDENT}property var ${safeName(name)}`);
  }
  for (const sym of table.values()) {
    if (sym.kind === "signal") {
      const init = resolveSignalInit(sym.name, sym.init, props.param ?? null, initScope);
      decls.push(`${INDENT}property var ${safeName(sym.name)}: ${init}`);
    } else if (sym.kind === "store") {
      // A store is a reactive `property var` holding its array/object; setters reassign it to rebind.
      const init = sym.init ? emitExpr(sym.init, initScope) : "undefined";
      decls.push(`${INDENT}property var ${safeName(sym.name)}: ${init}`);
    } else if (sym.kind === "memo") {
      if (t.isBlockStatement(sym.body)) {
        // Block-bodied memo: emit as an IIFE so the property binding reads reactive cells BARE.
        // `const visible = createMemo(() => { const f = filter(); return items.filter(...) })`
        // →  `readonly property var visible: (function() { var f = filter; return items.filter(...) })()`
        // emitBindingStmt reads signals bare (no __self.) so QML's binding tracker sees the deps.
        const bindStmtScope: Scope = { table, mode: "binding", jsImports, ...(baseLocals ? { locals: baseLocals } : {}) };
        const body = sym.body.body.map((s) => emitBindingStmt(s, bindStmtScope)).join(" ");
        decls.push(`${INDENT}readonly property var ${safeName(sym.name)}: (function() { ${body} })()`);
      } else {
        const binding = emitExpr(sym.body, { table, mode: "binding", jsImports, ...(baseLocals ? { locals: baseLocals } : {}) });
        decls.push(`${INDENT}readonly property var ${safeName(sym.name)}: ${binding}`);
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
    decls.push(`${INDENT}${kind} var ${safeName(name)}: ${emitExpr(init, initScope)}`);
  }
  // Module-const data tables (collected above; aliased in scope as __const_NAME).
  for (const [name, init] of moduleConstDecls)
    decls.push(`${INDENT}readonly property var ${constAliases[name]}: ${emitExpr(init, initScope)}`);
  // Mutable locals (`let x = <literal>` mutated in handlers): emit as `property var x: <init>` so
  // handler closures share a persistent slot on the instance rather than a local var in onCompleted.
  for (const [name, init] of mutableLocalsMap) {
    decls.push(`${INDENT}property var ${safeName(name)}: ${emitExpr(init, initScope)}`);
  }
  // A resource owns three properties: the value (`r`), a loading flag (`r_loading`, starts true so
  // the Suspense fallback shows before the first fetch settles), and an error slot (`r_error`). A
  // hidden `__load_<r>` property holds the (re)loader closure so both onCompleted and the source
  // Connections can invoke the same loader.
  for (const r of resources) {
    const rn = safeName(r.name);
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
      decls.push(`${INDENT}property var ${safeName(name)}: ${emitValue(node, fnScope)}`);
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
    for (const [name, info] of helpersMap) {
      const paramStr = info.params.map(safeName).join(", "); // safeName so a reserved param (e.g. `id`→`id_`) matches its body refs
      // The helper's own params shadow any same-named const alias / mutable local inherited above —
      // real JS lexical scoping (e.g. `function f(RULE_FIELDS) { … }` must NOT rewrite its own param).
      const bodyScope = info.params.length
        ? { ...helperScope, locals: { ...(helperScope.locals ?? {}), ...Object.fromEntries(info.params.map((p) => [p, safeName(p)])) } }
        : helperScope;
      const body = info.body.body.map((st) => emitStmt(st, bodyScope)).join(" ");
      decls.push(`${INDENT}function ${safeName(name)}(${paramStr}) { ${body} }`);
    }
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
      ...(Object.keys(constAliases).length ? { locals: constAliases } : {}),
    };
    for (const [name, info] of moduleFnDecls) {
      const paramStr = info.params.map(safeName).join(", ");
      const bodyScope = info.params.length
        ? { ...moduleFnScope, locals: { ...(moduleFnScope.locals ?? {}), ...Object.fromEntries(info.params.map((p) => [p, safeName(p)])) } }
        : moduleFnScope;
      const body = info.body.body.map((st) => emitStmt(st, bodyScope)).join(" ");
      decls.push(`${INDENT}function ${safeName(name)}(${paramStr}) { ${body} }`);
    }
  }
  // init(self) sidecar (inline): non-render setup statements + onMount bodies run in
  // Component.onCompleted on __self; onCleanup(fn) registrations collect into __cleanups and run in
  // Component.onDestruction. Imperative context → handler mode (signal reads bare, setters assign).
  const { setup, onMount, effects } = analyzeSetup(fn, mutableLocals, renderLocals);
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
  const setupScope: Scope = { table, mode: "handler", propsParam: props.param ?? undefined, propAliases, components, locals: { ...(baseLocals ?? {}), ...refLocals }, jsImports, ...(mutableLocals ? { mutableLocals } : {}) };
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
    .map((name) => `${safeName(name)} = ${safeName(name)};`);
  const onCompletedBody: string[] = [
    // Hoisted module-level statements (already emitted in their own module's import scope):
    // import-time code runs before any component setup.
    ...(moduleInit ?? []),
    ...freezeRenderLocals,
    ...filteredSetup.map((s) => emitStmt(s, setupScope)),
    ...onMount.map((b) => t.isBlockStatement(b)
      ? b.body.map((st) => emitStmt(st, setupScope)).join(" ")
      : `${emitExpr(b, setupScope)};`),
  ];
  // createEffect: run the body once in onCompleted, re-run via Connections on each reactive dep it reads.
  for (const body of effects) {
    onCompletedBody.push(t.isBlockStatement(body)
      ? body.body.map((s) => emitStmt(s, setupScope)).join(" ")
      : `${emitValue(body, setupScope)};`);
  }
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
      const rn = safeName(r.name);
      const srcSym = table.get(r.source);
      const srcRef = safeName(r.source); // the source signal — bare property read
      loaderStmts.push(
        `__load_${rn} = function() { ${rn}_loading = true; ${rn}_error = undefined; ` +
          `Promise.resolve(${safeName(r.fetcher)}(${srcRef})).then(function(v) { ${rn} = v; ${rn}_loading = false; })` +
          `.catch(function(e) { ${rn}_error = e; ${rn}_loading = false; }); };`,
        `__load_${rn}();`,
      );
      // Reload when the source signal changes (only if the source is a tracked reactive cell).
      if (srcSym && (srcSym.kind === "signal" || srcSym.kind === "memo")) {
        const cap = r.source.charAt(0).toUpperCase() + r.source.slice(1);
        conns.push(
          `${INDENT}Connections {`,
          `${INDENT}${INDENT}target: __self`,
          `${INDENT}${INDENT}function on${cap}Changed() { __load_${rn}(); }`,
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
  ]);
  for (const body of effects) {
    const bodyDeps = effectDeps(body, table, {
      propsParam: props.param,
      propAliases,
      ctxBindings: hasCtxBindings ? ctxBindings : undefined,
      ctxValueShape: ctx?.ctxValueShape,
      refLocals,
      knownNonReactive: effectKnownNonReactive,
      helpers: helpersMap,
    });
    for (const dep of bodyDeps) {
      const cap = dep.prop.charAt(0).toUpperCase() + dep.prop.slice(1);
      const run = t.isBlockStatement(body)
        ? body.body.map((s) => emitStmt(s, setupScope)).join(" ")
        : `${emitValue(body, setupScope)};`;
      conns.push(
        `${INDENT}Connections {`,
        `${INDENT}${INDENT}target: ${dep.target}`,
        `${INDENT}${INDENT}function on${cap}Changed() { ${run} }`,
        `${INDENT}}`,
      );
    }
  }
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
    lifecycle.push(`${INDENT}Component.onDestruction: { for (var i = 0; i < __cleanups.length; i++) __cleanups[i](); }`);
  }
  lifecycle.push(...conns);

  if (openIdx >= 0) lines.splice(openIdx + 1, 0, ...decls, ...lifecycle);
  return lines;
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
function validateChildrenSlot(render: t.CallExpression, propsParam: string | null): void {
  if (!propsParam) return;
  const isMarker = (n: t.Node): boolean => t.isMemberExpression(n) && !n.computed &&
    t.isIdentifier(n.object, { name: propsParam }) && t.isIdentifier(n.property, { name: "children" });
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

/** Module-level `const NAME = <pure literal>` declarations — static data tables usable as
 *  Repeater models / binding sources. Anything with calls, references or spreads is skipped. */
function moduleLevelConsts(file: t.File): Map<string, t.Expression> {
  const out = new Map<string, t.Expression>();
  for (const st of file.program.body) {
    if (!t.isVariableDeclaration(st) || st.kind !== "const") continue;
    for (const d of st.declarations) {
      if (t.isIdentifier(d.id) && d.init && isPureLiteral(d.init)) out.set(d.id.name, d.init);
    }
  }
  return out;
}

function isPureLiteral(n: t.Node): boolean {
  if (t.isStringLiteral(n) || t.isNumericLiteral(n) || t.isBooleanLiteral(n) || t.isNullLiteral(n)) return true;
  if (t.isUnaryExpression(n) && n.operator === "-") return isPureLiteral(n.argument);
  if (t.isTemplateLiteral(n)) return n.expressions.length === 0;
  if (t.isArrayExpression(n))
    return n.elements.every((e) => e != null && !t.isSpreadElement(e) && isPureLiteral(e));
  if (t.isObjectExpression(n))
    return n.properties.every((p) => t.isObjectProperty(p) && !p.computed
      && (t.isIdentifier(p.key) || t.isStringLiteral(p.key))
      && t.isExpression(p.value) && isPureLiteral(p.value));
  return false;
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
