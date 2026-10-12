import _traverse from "@babel/traverse";
import * as t from "@babel/types";
import { hParts, isHCall } from "../ast/h.ts";
import { safeName } from "../names/safe.ts";

const traverse: typeof _traverse = (_traverse as any).default ?? _traverse;

/** What a name in a component means. */
export type Sym =
  | { kind: "signal"; name: string; setter: string; init: t.Expression | null }
  | { kind: "setter"; signal: string }
  | { kind: "store"; name: string; setter: string; init: t.Expression | null }
  | { kind: "storeSetter"; store: string }
  | { kind: "memo"; name: string; body: t.Expression | t.BlockStatement;
      /** Set when the memo needs its own state (a `prev` param, an initial value, or `on(...)`):
       *  the user's function, called with the previous value. */
      fn?: t.ArrowFunctionExpression | t.FunctionExpression; init?: t.Expression; on?: OnSpec }
  | { kind: "selector"; name: string; source: t.Expression; fn?: t.Expression }
  | { kind: "resource"; name: string; source: string; fetcher: string }
  | { kind: "derived"; name: string; params: string[]; body: t.Expression | t.BlockStatement };

export type SymbolTable = Map<string, Sym>;

/**
 * Scan a component function body for reactive declarations:
 *  - `const [g, s] = createSignal(init)` → a signal (g) + its setter (s)
 *  - `const m = createMemo(() => …)`     → a memo (a readonly derived property)
 *  - `const d = (…) => <expr>`           → a derived accessor (inlined where called, stays reactive)
 *    — UNLESS it has a non-call reference elsewhere in the body (see `hasNonCallReference`), in
 *    which case it is promoted to a real helper method instead (analyzeHelpers picks it up) and is
 *    deliberately left OUT of this table.
 */
export function analyzeSignals(fn: t.Function): SymbolTable {
  const table: SymbolTable = new Map();
  // Zero-param expression-body arrow candidates found during the main pass below, kept alongside
  // their OWN declarator id node (not a clone — same object as in `fn.body`) so the promotion check
  // after the pass can ask Babel's scope resolution "does THIS read bind to THIS declaration" —
  // see hasNonCallReference's doc comment for why that matters (shadowing).
  const derivedDeclIds = new Map<string, t.Identifier>();
  traverse(
    t.file(t.program(fn.body.type === "BlockStatement" ? fn.body.body : [t.expressionStatement(fn.body)])),
    {
      VariableDeclarator(path) {
        const { id, init } = path.node;
        if (!init) return;

        // const [r] = createResource(source, fetcher) — a resource accessor (r()) backed by an async loader.
        if (t.isArrayPattern(id) && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "createResource" })) {
          const r = id.elements[0];
          const [src, fetch] = init.arguments;
          if (t.isIdentifier(r) && t.isIdentifier(src) && t.isIdentifier(fetch))
            table.set(r.name, { kind: "resource", name: r.name, source: src.name, fetcher: fetch.name });
          return;
        }

        if (t.isArrayPattern(id) && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "createSignal" })) {
          const [g, s] = id.elements;
          if (t.isIdentifier(g)) {
            const initArg = init.arguments[0];
            // Getter-only destructure (`const [x] = createSignal(v)`) is valid Solid: the value is
            // still per-instance reactive state — a synthesized setter name keeps the emit uniform.
            const setterName = t.isIdentifier(s) ? s.name : `__set_${g.name}`;
            table.set(g.name, { kind: "signal", name: g.name, setter: setterName, init: initArg && t.isExpression(initArg) ? initArg : null });
            table.set(setterName, { kind: "setter", signal: g.name });
          }
          return;
        }

        // createStore: a reactive `property var` holding an array/object. QML can't track in-place
        // mutation of a `var`, so reactivity is reassign-to-rebind: every setter form produces a NEW
        // value and assigns it (the property's change signal fires → bindings/<For> re-evaluate).
        if (t.isArrayPattern(id) && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "createStore" })) {
          const [g, s] = id.elements;
          if (t.isIdentifier(g) && t.isIdentifier(s)) {
            const initArg = init.arguments[0];
            table.set(g.name, { kind: "store", name: g.name, setter: s.name, init: initArg && t.isExpression(initArg) ? initArg : null });
            table.set(s.name, { kind: "storeSetter", store: g.name });
          }
          return;
        }

        if (!t.isIdentifier(id)) return;

        if (t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "createMemo" })) {
          const r = parseReactionArgs(init, "createMemo");
          const initVal = init.arguments[1] && t.isExpression(init.arguments[1]) ? init.arguments[1] : undefined;
          if (!r.on && !initVal && r.fn.params.length === 0)
            table.set(id.name, { kind: "memo", name: id.name, body: r.fn.body });
          else
            table.set(id.name, { kind: "memo", name: id.name, body: r.fn.body, fn: r.fn, init: initVal, on: r.on });
          return;
        }

        // const isSel = createSelector(source, fn?) — `isSel(k)` is `fn(k, source())` (default ===).
        if (t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "createSelector" })) {
          const [src, cmp] = init.arguments;
          if (!src || !t.isExpression(src)) throw new Error("createSelector(source, fn?): the source must be an accessor");
          if (cmp && !t.isExpression(cmp)) throw new Error("createSelector(source, fn?): the comparator must be a function");
          table.set(id.name, { kind: "selector", name: id.name, source: src, ...(cmp ? { fn: cmp as t.Expression } : {}) });
          return;
        }

        // Only zero-param, expression-body arrows become derived accessors (inlined at call sites).
        // Param-having or block-bodied arrows are helpers (analyzeHelpers picks them up as methods).
        if (t.isArrowFunctionExpression(init) && !t.isJSXElement(init.body) && !t.isJSXFragment(init.body)
            && init.params.length === 0 && !t.isBlockStatement(init.body)) {
          table.set(id.name, { kind: "derived", name: id.name, params: [], body: init.body });
          derivedDeclIds.set(id.name, id);
        }
      },
    },
  );
  // Promotion pass: a derived accessor referenced by VALUE anywhere else in the component (passed
  // to another call, stored, returned, used as a JSX prop, …) has no call site to inline INTO —
  // the reference itself would otherwise emit as a bare, undeclared name (a QML
  // "ReferenceError: <name> is not defined" at runtime, since nothing ever declares it). Delete it
  // from the table so it is no longer treated as "derived" (inlined) anywhere downstream;
  // analyzeHelpers makes the IDENTICAL decision (same predicate) and picks it up as a real method
  // instead — the two analyses can never disagree about which path a name takes.
  for (const [name, declId] of derivedDeclIds) {
    if (hasNonCallReference(fn, name, declId)) table.delete(name);
  }
  return table;
}

/** True if `name` — a zero-param expression-body arrow candidate declared at `declId`
 *  (`const <name> = () => <expr>`) — has at least one use ELSEWHERE in the component body that is
 *  NOT a direct call (`name(...)`): passed by reference (`register(f)`), used as a JSX handler
 *  (`onClick={f}`), handed to a callback-taking call (`setTimeout(f, 0)`, `arr.map(f)`), stored
 *  (`{cb: f}`), or returned. Such a use has no "call site" an inlined expression could sit in —
 *  only a real, callable QML value does, so these are the uses that force promotion to a method
 *  (see analyzeSignals' and analyzeHelpers' own comments on this function).
 *
 *  Shadow-aware: a NESTED declaration/param reusing the same name (`arr.map(function(f){ return
 *  f.id; })`) binds its OWN `f`; reads inside that nested scope refer to the shadow, not this
 *  candidate, and must not count. Resolved via Babel's own scope crawl (not the ad-hoc shadow-set
 *  style used elsewhere in this file, e.g. `effectDeps`'s `shadowed`): each candidate read's
 *  resolved binding identifier is compared against `declId` — the exact AST node of this
 *  candidate's own `const <name> = …` id (same node object as in `fn.body`, not a clone), so a
 *  shadowed read (resolving to some OTHER binding) is correctly skipped rather than over- or
 *  under-counted. */
export function hasNonCallReference(fn: t.Function, name: string, declId: t.Identifier): boolean {
  const stmts = fn.body.type === "BlockStatement" ? fn.body.body : [t.expressionStatement(fn.body)];
  const file = t.file(t.program(stmts));
  let found = false;
  traverse(file, {
    Identifier(path) {
      if (found) return;
      if (path.node.name !== name) return;
      if (path.node === declId) return; // the declaration's own binding identifier, not a use
      // A property-KEY position (`{x: ...}`) or a non-computed member's property (`obj.x`) is not a
      // value read of a same-named candidate (readNamesIn applies the identical skip, above).
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      if (t.isObjectProperty(path.parent) && !path.parent.computed && path.parent.key === path.node) return;
      const binding = path.scope.getBinding(name);
      if (!binding || binding.identifier !== declId) return; // shadowed by a nested local/param
      const isDirectCall = t.isCallExpression(path.parent) && path.parent.callee === path.node;
      if (!isDirectCall) { found = true; path.stop(); }
    },
  });
  return found;
}

/** Wrap an expression-bodied arrow/function body as a `return <expr>;` block — the expression IS
 *  the return value, so a bare `ExpressionStatement` would silently make the helper return
 *  `undefined`. Shared by analyzeHelpers' own param/block-arrow path and its promoted-derived path
 *  (a zero-param expression arrow referenced by value — see `hasNonCallReference`) — identical
 *  shape, one place owns it. */
function exprArrowToHelperBody(body: t.Expression | t.BlockStatement): t.BlockStatement {
  return t.isBlockStatement(body) ? body : t.blockStatement([t.returnStatement(body)]);
}

/** Top-level `const X = createContext(...)` declarations → the set of context names. */
export function analyzeContexts(ast: t.File): Set<string> {
  const out = new Set<string>();
  for (const node of ast.program.body) {
    if (!t.isVariableDeclaration(node)) continue;
    for (const d of node.declarations) {
      if (t.isIdentifier(d.id) && d.init && t.isCallExpression(d.init)
          && t.isIdentifier(d.init.callee, { name: "createContext" })) out.add(d.id.name);
    }
  }
  return out;
}

/** A context provider's analysis: the context it provides and the shape of its value object. A
 *  value member is `"accessor"` if its bound value is a signal/memo in the component's table (a
 *  reactive getter), else `"fn"` (a method to expose as a function property). The provider's render
 *  root must be `h(X.Provider, { value: V }, …)` with `V` an object literal — directly or bound to a
 *  local `const V = { … }` in the body. */
export interface ProviderInfo {
  ctx: string;
  valueShape: Record<string, "accessor" | "fn">;
  fnMembers: Record<string, t.Expression>;
  /** When `value={ident}` is bound to a local `const ident = {…}`, the local var name — so the
   *  declaration is filtered out of imperative setup (it is structural, not runtime logic). */
  valueLocal: string | null;
}
export function analyzeProvider(fn: t.Function, table: SymbolTable, contexts: Set<string>): ProviderInfo | null {
  if (!t.isBlockStatement(fn.body)) return null;
  const ret = fn.body.body.find((s): s is t.ReturnStatement => t.isReturnStatement(s));
  const call = ret?.argument;
  if (!isHCall(call)) return null;
  const { tag, props: propsArg } = hParts(call);
  if (!t.isMemberExpression(tag) || tag.computed || !t.isIdentifier(tag.object)
      || !t.isIdentifier(tag.property, { name: "Provider" }) || !contexts.has(tag.object.name)) return null;
  const ctx = tag.object.name;

  // Find the value object literal: either inline `value={ {...} }` or `value={ident}` where ident is
  // a local `const ident = { … }`.
  let valueObj: t.ObjectExpression | null = null;
  let valueLocal: string | null = null;
  if (t.isObjectExpression(propsArg)) {
    for (const p of propsArg.properties) {
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "value" })) {
        if (t.isObjectExpression(p.value)) valueObj = p.value;
        else if (t.isIdentifier(p.value)) { valueLocal = p.value.name; valueObj = findLocalObject(fn, p.value.name); }
      }
    }
  }
  if (!valueObj) throw new Error(`provider for ${ctx}: value must be an object literal (or a local const bound to one)`);

  const valueShape: Record<string, "accessor" | "fn"> = {};
  const fnMembers: Record<string, t.Expression> = {};
  for (const p of valueObj.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value))
      throw new Error(`provider for ${ctx}: value members must be simple keys`);
    const key = p.key.name;
    // shorthand `{ count }` → value is Identifier count; `{ increment: () => … }` → arrow.
    if (t.isIdentifier(p.value) && (() => { const s = table.get((p.value as t.Identifier).name); return s && (s.kind === "signal" || s.kind === "memo"); })()) {
      valueShape[key] = "accessor"; // already exposed as the provider's signal/memo property
    } else {
      valueShape[key] = "fn";
      fnMembers[key] = p.value; // emitted as `property var <key>: <fn>` by emitComponentType
    }
  }
  return { ctx, valueShape, fnMembers, valueLocal };
}

/** A consumer's `const c = useContext(Ctx)` bindings: the local name and the context it reads. */
export interface UseContextBinding { local: string; ctx: string; }
export function analyzeUseContext(fn: t.Function, contexts: Set<string>): UseContextBinding[] {
  const out: UseContextBinding[] = [];
  if (!t.isBlockStatement(fn.body)) return out;
  for (const s of fn.body.body) {
    if (!t.isVariableDeclaration(s)) continue;
    for (const d of s.declarations) {
      if (!t.isIdentifier(d.id) || !d.init || !t.isCallExpression(d.init)) continue;
      if (!t.isIdentifier(d.init.callee, { name: "useContext" })) continue;
      const arg = d.init.arguments[0];
      if (!t.isIdentifier(arg) || !contexts.has(arg.name))
        throw new Error(`useContext: argument must be a known context (got ${arg && t.isIdentifier(arg) ? arg.name : arg?.type})`);
      out.push({ local: d.id.name, ctx: arg.name });
    }
  }
  return out;
}

/** Resolve a `const name = { … }` object literal at the function body's top level. */
function findLocalObject(fn: t.Function, name: string): t.ObjectExpression | null {
  if (!t.isBlockStatement(fn.body)) return null;
  for (const s of fn.body.body) {
    if (!t.isVariableDeclaration(s)) continue;
    for (const d of s.declarations) {
      if (t.isIdentifier(d.id, { name }) && d.init && t.isObjectExpression(d.init)) return d.init;
    }
  }
  return null;
}

/** `on(deps, fn, { defer })` — explicit dependencies: only `deps` is tracked, `fn` runs untracked
 *  with `(input, prevInput, prevValue)`. */
export interface OnSpec { deps: t.Expression; defer: boolean }

/** A computation declared at a component's top level: `createEffect` (runs after mount, in source
 *  order with onMount), `createRenderEffect`/`createComputed` (run at their position in setup). */
export interface EffectInfo {
  kind: "effect" | "renderEffect" | "computed";
  /** The user function, called with the previous value (`fn(prev)`; for `on`, `fn(input, prevInput, prev)`). */
  fn: t.ArrowFunctionExpression | t.FunctionExpression;
  /** The second argument (`createEffect(fn, init)`): the first run's `prev`. */
  init?: t.Expression;
  on?: OnSpec;
  /** The source statement (a render effect/computed keeps its position in setup). */
  stmt: t.Statement;
}

const REACTION_APIS: Record<string, EffectInfo["kind"]> = { createEffect: "effect", createRenderEffect: "renderEffect", createComputed: "computed" };

/** The function a createEffect/createMemo/… receives: an inline function, `on(deps, fn, opts)`, or a
 *  named function (`createEffect(sync)` → `(prev) => sync(prev)`). Anything else fails loud. */
export function parseReactionArgs(call: t.CallExpression, api: string): { fn: t.ArrowFunctionExpression | t.FunctionExpression; on?: OnSpec } {
  const arg = call.arguments[0];
  const asFn = (a: t.Node | undefined, what: string): t.ArrowFunctionExpression | t.FunctionExpression => {
    if (a && (t.isArrowFunctionExpression(a) || t.isFunctionExpression(a))) {
      if (a.async) throw new Error(`${what}: an async function is not supported — Solid tracks only the synchronous part; read the signals first, then call an async helper`);
      return a;
    }
    if (a && (t.isIdentifier(a) || t.isMemberExpression(a))) {
      const p = t.identifier("__p");
      return t.arrowFunctionExpression([p], t.callExpression(a, [p]));
    }
    throw new Error(`${what}: expected a function, got ${a ? a.type : "nothing"}`);
  };
  if (arg && t.isCallExpression(arg) && t.isIdentifier(arg.callee, { name: "on" })) {
    const [deps, fn, opts] = arg.arguments;
    if (!deps || !t.isExpression(deps)) throw new Error(`${api}(on(deps, fn)): deps must be an accessor or an array of accessors`);
    let defer = false;
    if (opts) {
      if (!t.isObjectExpression(opts)) throw new Error(`${api}(on(deps, fn, opts)): opts must be an object literal ({ defer: true })`);
      for (const p of opts.properties) {
        if (!t.isObjectProperty(p) || !t.isIdentifier(p.key, { name: "defer" }) || !t.isBooleanLiteral(p.value))
          throw new Error(`${api}(on(deps, fn, opts)): the only supported option is a literal \`defer: true|false\``);
        defer = p.value.value;
      }
    }
    return { fn: asFn(fn, `${api}(on(deps, fn))`), on: { deps, defer } };
  }
  return { fn: asFn(arg, api) };
}

/** `on`'s deps read once: `a` → `a()`, `[a, b]` → `[a(), b()]` (each dep is an accessor). The
 *  expression whose reads are the computation's ONLY dependencies. */
export function onDepsCall(deps: t.Expression): t.Expression {
  if (t.isArrayExpression(deps))
    return t.arrayExpression(deps.elements.map((e) => {
      if (!e || !t.isExpression(e)) throw new Error("on([deps…]): every dep must be an accessor");
      return t.callExpression(e, []);
    }));
  return t.callExpression(deps, []);
}

/** A component's non-render lifecycle logic: imperative setup statements (everything at the body's
 *  top level that is NOT a reactive declaration, an onMount call, a createEffect call, or the render
 *  return), the onMount callback bodies and the computations (`effectInfos`). createRenderEffect/
 *  createComputed statements STAY in `setup` (they run at their position, as in Solid); `afterMount`
 *  lists onMount bodies and createEffects in source order (Solid queues both as effects, run after
 *  the render in creation order). `effects` = the plain createEffect bodies (kept for callers that
 *  only need the bodies).
 *
 *  `mutableLocals` (from analyzeMutableLocals) are excluded from setup: they become `property var`
 *  declarations on the instance, so their initializer is already emitted as a property value and
 *  the statement must not also appear in onCompleted. */
export function analyzeSetup(fn: t.Function, mutableLocals?: Set<string>, renderLocals?: Set<string>): {
  setup: t.Statement[]; onMount: t.Node[]; effects: t.Node[]; effectInfos: EffectInfo[];
  afterMount: Array<{ mount: t.Node } | { effect: number }>;
} {
  const setup: t.Statement[] = [];
  const onMount: t.Node[] = [];
  const effects: t.Node[] = [];
  const effectInfos: EffectInfo[] = [];
  const afterMount: Array<{ mount: t.Node } | { effect: number }> = [];
  if (!t.isBlockStatement(fn.body)) return { setup, onMount, effects, effectInfos, afterMount };
  for (const s of fn.body.body) {
    if (t.isReturnStatement(s)) continue; // the render
    // FunctionDeclarations are helpers (emitted as QML object methods, not setup statements)
    if (t.isFunctionDeclaration(s)) continue;
    // a reactive declaration (createSignal/createMemo, or a derived arrow) → handled by analyzeSignals
    if (t.isVariableDeclaration(s) && s.declarations.some(isReactiveDecl)) continue;
    // a mutable local (let x = <literal>, mutated later) → promoted to property var; skip from setup
    if (mutableLocals && t.isVariableDeclaration(s) && s.declarations.every(
      (d) => t.isIdentifier(d.id) && mutableLocals.has(d.id.name)
    )) continue;
    // a render local (`const x = expr` read by JSX) → promoted to readonly property var; skip setup
    if (renderLocals && t.isVariableDeclaration(s) && s.declarations.every(
      (d) => t.isIdentifier(d.id) && renderLocals.has(d.id.name)
    )) continue;
    // onMount(fn) → collect the callback body, run after setup
    if (t.isExpressionStatement(s) && t.isCallExpression(s.expression)
        && t.isIdentifier(s.expression.callee, { name: "onMount" })) {
      const cb = s.expression.arguments[0];
      if (!cb || !(t.isArrowFunctionExpression(cb) || t.isFunctionExpression(cb)))
        throw new Error("onMount(fn): expected an inline function");
      onMount.push(cb.body);
      afterMount.push({ mount: cb.body });
      continue;
    }
    // createEffect / createRenderEffect / createComputed
    if (t.isExpressionStatement(s) && t.isCallExpression(s.expression) && t.isIdentifier(s.expression.callee)
        && s.expression.callee.name in REACTION_APIS) {
      const call = s.expression;
      const api = (call.callee as t.Identifier).name;
      const kind = REACTION_APIS[api];
      const r = parseReactionArgs(call, api);
      const init = call.arguments[1] && t.isExpression(call.arguments[1]) ? call.arguments[1] : undefined;
      effectInfos.push({ kind, fn: r.fn, ...(init ? { init } : {}), ...(r.on ? { on: r.on } : {}), stmt: s });
      if (kind === "effect") {
        if (!r.on) effects.push(r.fn.body);
        afterMount.push({ effect: effectInfos.length - 1 });
      } else {
        setup.push(s); // runs at its position (the emitter maps the statement to its computation)
      }
      continue;
    }
    setup.push(s);
  }
  return { setup, onMount, effects, effectInfos, afterMount };
}

/** Identifier reads of any name in `names` inside `expr` — skipping a property KEY position
 *  (`obj.x` / `{x: ...}`, non-computed), which is not a value read of a same-named candidate.
 *  Shared by the render-usage scan and the inter-candidate reference scan below. Also accepts a
 *  BlockStatement directly (a memo/helper/effect body), not just a bare expression. */
function readNamesIn(expr: t.Expression | t.BlockStatement, names: Set<string>): Set<string> {
  const found = new Set<string>();
  const program = t.isBlockStatement(expr) ? t.program(expr.body) : t.program([t.expressionStatement(expr)]);
  traverse(t.file(program), {
    Identifier(path) {
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      if (t.isObjectProperty(path.parent) && !path.parent.computed && path.parent.key === path.node) return;
      if (names.has(path.node.name)) found.add(path.node.name);
    },
  });
  return found;
}

/** Every OTHER position in a component whose expression is evaluated EAGERLY, as part of a QML
 *  binding or a detached function body — NOT sequentially inside Component.onCompleted, where a
 *  plain `var` declared earlier in the same block would already be in scope. A render-local read
 *  ONLY from one of these is just as broken as one read only from `render` itself (the gap
 *  analyzeRenderLocals existed to close for `render` — see its own doc comment): the name is a
 *  `var` local to onCompleted's own anonymous function, invisible to any OTHER function.
 *
 *   - createMemo bodies, createSignal/createStore initial values, a zero-arg "derived" accessor's
 *     body — all become a QML property's binding expression, evaluated at CONSTRUCTION, before
 *     onCompleted ever runs.
 *   - createResource's `source` — just the bare name itself; it is read directly inside
 *     onCompleted's loader statements (safe), but ALSO inside the Connections re-run handler the
 *     loader wires for a source that is itself a signal/memo — a SEPARATE function, same
 *     disconnected-scope problem as createEffect below.
 *   - createEffect bodies — the ONE-TIME run happens inside onCompleted itself (textually after
 *     every plain setup statement — verified by reading component.ts's assembly order, so that
 *     part is safe), but a DEPENDENT effect ALSO gets a `Connections { function on<Dep>Changed()
 *     {…} }` re-run handler, which is its own top-level QML function — NOT nested inside
 *     onCompleted, so it has no closure over onCompleted's local `var`s at all. (onMount bodies
 *     have no such re-run handler, so they are NOT included here — they stay in onCompleted,
 *     sequential and safe.)
 *   - every local helper's body (analyzeHelpers) — emitted as a method ON THE COMPONENT OBJECT,
 *     again not nested inside onCompleted, reachable from a binding (`text: helperFn()`) OR a
 *     handler either way the method itself cannot see onCompleted's locals.
 *
 *  A context provider's own value-local object is NOT special-cased here: it is already named
 *  directly in `render` (`<Ctx.Provider value={counter}>`), so the existing render-seed already
 *  reaches it, and the existing inter-candidate BFS already follows ITS initializer for further
 *  render-locals it reads — no separate treatment needed. */
function eagerBindingSeeds(table: SymbolTable, helpers: Map<string, HelperInfo>, names: Set<string>): Set<string> {
  const found = new Set<string>();
  const add = (s: Set<string>) => { for (const n of s) found.add(n); };
  for (const sym of table.values()) {
    if (sym.kind === "memo") {
      add(readNamesIn(sym.body, names));
      if (sym.on) add(readNamesIn(sym.on.deps, names));
      if (sym.init) add(readNamesIn(sym.init, names));
    } else if (sym.kind === "selector") {
      add(readNamesIn(sym.source, names));
      if (sym.fn) add(readNamesIn(sym.fn, names));
    }
    else if ((sym.kind === "signal" || sym.kind === "store") && sym.init) add(readNamesIn(sym.init, names));
    else if (sym.kind === "derived") add(readNamesIn(sym.body, names));
    else if (sym.kind === "resource" && names.has(sym.source)) found.add(sym.source);
  }
  for (const helper of helpers.values()) add(readNamesIn(helper.body, names));
  return found;
}

/** `createEffect(fn)` callback bodies at the top level of a component — a LOCAL re-scan (not
 *  shared with analyzeSetup's bucketing, which also needs `renderLocals` as an input and so runs
 *  too late to seed it) used only to feed eagerBindingSeeds' effect-body reasoning above. */
function collectEffectBodies(fn: t.Function): (t.Expression | t.BlockStatement)[] {
  const out: (t.Expression | t.BlockStatement)[] = [];
  if (!t.isBlockStatement(fn.body)) return out;
  for (const s of fn.body.body) {
    if (!t.isExpressionStatement(s) || !t.isCallExpression(s.expression)) continue;
    if (!t.isIdentifier(s.expression.callee) || !(s.expression.callee.name in REACTION_APIS)) continue;
    // Every argument (the function, `on(deps, fn)`, the initial value): each computation is its own
    // method, so a local it reads must be a property, like for a re-run handler.
    for (const a of s.expression.arguments) if (t.isExpression(a)) out.push(a);
  }
  return out;
}

/** Top-level immutable locals whose values are read by the render tree — directly, OR
 *  transitively through another such local's OWN initializer (`const fonts = f(); const opts =
 *  fonts.map(...); return <Select options={opts}/>` — `fonts` is never named in `render`, only
 *  inside `opts`'s initializer).
 *
 *  They must be QML properties, not `var` declarations inside Component.onCompleted, because render
 *  bindings are evaluated outside that lifecycle closure (verified against the real engine: a
 *  property binding referencing a name only assigned in Component.onCompleted throws/undefineds —
 *  onCompleted runs AFTER the object's own property bindings are first evaluated). This covers
 *  code like:
 *    const encoded = Base64.encode("solid-qml");
 *    return <text>{encoded}</text>
 *
 *  Also covers a `let` with the same shape, AS LONG AS it is never reassigned (same snapshot
 *  semantics as `const`); `mutableLocals` (already-promoted literal-init reassigned lets, from
 *  analyzeMutableLocals) is excluded here to avoid double-booking.
 *
 *  A `let` that IS reassigned somewhere, selected here (so its init isn't the simple-literal shape
 *  analyzeMutableLocals already promotes), FAILS LOUD instead of silently emitting wrong semantics
 *  — as a `readonly property var` it would freeze at its first value, or silently drop the
 *  reassignment if emitted non-readonly instead.
 *
 *  Does NOT decide which of these must be FROZEN once (a signal/memo/resource/prop read in the
 *  initializer) — see renderLocalLiveTaint below, called separately once the caller (component.ts)
 *  has removed its own deliberate exception (a context provider's value-local) from the map.
 *
 *  `table`/`helpers` (optional, passed by component.ts — it already has both before calling this)
 *  seed `used` from every OTHER eagerly-evaluated position too, not just `render` itself — see
 *  eagerBindingSeeds' own doc comment. Omitting them keeps the OLD render-only seeding (every
 *  existing call site that does not pass them is unaffected). */
export function analyzeRenderLocals(fn: t.Function, render: t.CallExpression, mutableLocals?: Set<string>, table?: SymbolTable, helpers?: Map<string, HelperInfo>): Map<string, t.Expression> {
  const candidates = new Map<string, t.Expression>();
  const letCandidates = new Set<string>();
  if (!t.isBlockStatement(fn.body)) return candidates;

  for (const s of fn.body.body) {
    if (t.isReturnStatement(s)) break;
    if (!t.isVariableDeclaration(s) || s.kind === "var") continue; // const or let only
    if (s.declarations.length !== 1) continue;
    const d = s.declarations[0];
    if (!t.isIdentifier(d.id) || !d.init || !t.isExpression(d.init)) continue;
    if (isReactiveDecl(d)) continue;
    if (t.isArrowFunctionExpression(d.init) || t.isFunctionExpression(d.init)) continue;
    if (mutableLocals?.has(d.id.name)) continue; // already its own mutable `property var`
    candidates.set(d.id.name, d.init);
    if (s.kind === "let") letCandidates.add(d.id.name);
  }
  if (candidates.size === 0) return candidates;

  const names = new Set(candidates.keys());

  // Seed from direct render usage AND every other eager-binding position (memo/signal/store
  // inits, derived accessors, resource sources, dependent-effect re-run bodies, helper bodies —
  // see eagerBindingSeeds), then close over inter-candidate references (BFS).
  const used = readNamesIn(render, names);
  if (table) for (const n of eagerBindingSeeds(table, helpers ?? new Map(), names)) used.add(n);
  for (const effectBody of collectEffectBodies(fn)) for (const n of readNamesIn(effectBody, names)) used.add(n);
  const queue = [...used];
  while (queue.length) {
    const cur = queue.pop()!;
    for (const dep of readNamesIn(candidates.get(cur)!, names)) {
      if (!used.has(dep)) { used.add(dep); queue.push(dep); }
    }
  }

  for (const name of [...candidates.keys()]) if (!used.has(name)) candidates.delete(name);

  if (letCandidates.size > 0) {
    const reassigned = new Set<string>();
    traverse(t.file(t.program(fn.body.body)), {
      AssignmentExpression(path) {
        const { left } = path.node;
        if (t.isIdentifier(left) && letCandidates.has(left.name)) reassigned.add(left.name);
      },
      UpdateExpression(path) {
        const { argument } = path.node;
        if (t.isIdentifier(argument) && letCandidates.has(argument.name)) reassigned.add(argument.name);
      },
    });
    for (const name of used) {
      if (reassigned.has(name))
        throw new Error(`let ${name}: reassigned later but read by a render binding, with a non-literal initializer — give it a literal initial value (so analyzeMutableLocals promotes it to a real mutable property) or avoid reassigning it; as emitted here it would either freeze at its first value (readonly) or silently drop the reassignment (read-only property write).`);
    }
  }

  return candidates;
}

/** Which render-locals must be FROZEN once, right after construction, instead of staying a live
 *  QML binding forever. Called SEPARATELY (component.ts) from analyzeRenderLocals, AFTER a context
 *  provider's own value-local (`provider?.valueLocal`) has been deleted from the map — that one is
 *  a DELIBERATE exception: `const counter = { count, increment: … }` passed as `<Ctx.Provider
 *  value={counter}>` is EXPECTED to stay live (every descendant's `counter.count()` must track the
 *  signal — that is the entire point of the context value object; proven by the shipped context
 *  golden tests, which read a signal bare inside an object literal assigned to such a const and
 *  rely on it staying reactive).
 *
 *  Solid evaluates a plain const/let ONCE, even if its initializer reads a signal/memo/resource or
 *  a prop — unlike createMemo, it does NOT re-run when that cell changes. But this transpiler
 *  represents EVERY such reference (called or not) as a bare, live QML property read (cellRef) —
 *  so emitting one of these as a plain `readonly property var` binding would make it silently LIVE
 *  (re-running whenever the thing it reads changes), diverging from Solid. There is no snapshot
 *  that is both synchronously available (render needs the value at construction time — the eager
 *  evaluation analyzeRenderLocals exists for) and untracked within the binding expression itself
 *  (verified against the real engine: wrapping the read in an IIFE does not stop it from
 *  re-evaluating). So instead: emit it as a NON-readonly `property var` (still bound to the same
 *  expression, so it gets the correct value at construction — ordering unaffected), then — in
 *  Component.onCompleted, before any user onMount/effect code — assign it to itself (`x = x`).
 *  Assigning a VALUE to a QML property always clears any binding on it (verified against the real
 *  engine: after `snap = snap`, changing the signal the binding used to read no longer has any
 *  effect on `snap`, even transitively through another frozen local that reads `snap`) — exactly
 *  Solid's "evaluated once, frozen forever" snapshot.
 *
 *  `isLive(name)` tells the caller which bare names denote such live, externally-mutable state
 *  (signals/memos/resources via the SymbolTable, OR the props param / a prop alias) — component.ts
 *  knows both; this function stays agnostic of how `isLive` decides. A render-local that reads
 *  ANOTHER render-local already found to be tainted is tainted too (transitively) — processed in
 *  Map iteration order, which is declaration order, so a dependency's taint is already known by
 *  the time a later candidate that reads it is checked (no forward references are possible in a
 *  straight-line top-level body). */
export function renderLocalLiveTaint(renderLocals: Map<string, t.Expression>, isLive: (name: string) => boolean): Set<string> {
  const tainted = new Set<string>();
  for (const [name, init] of renderLocals) {
    let live = false;
    traverse(t.file(t.program([t.expressionStatement(init)])), {
      Identifier(path) {
        if (live) return;
        if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
        if (isLive(path.node.name) || tainted.has(path.node.name)) live = true;
      },
    });
    if (live) tainted.add(name);
  }
  return tainted;
}

/** A single reactive dependency an effect body reads: `target` is the QML object the Connections
 *  block attaches to (`__self` for the component's own properties, `__ctx_<ctx>` for a consumed
 *  context's provider instance, or a bare/dotted QML expression — a context property, an imported
 *  singleton, a ref's element id, or a prefix of a longer member chain), `prop` is the property
 *  whose `on<Prop>Changed` handler re-runs the effect. */
export interface EffectDep { target: string; prop: string }

/** JS/QML globals that are never QObjects with a NOTIFY'd member — reading `Math.max`, `JSON.parse`,
 *  `console.log`, `globalThis.x`, `Qt.rgba`, etc. inside an effect must NOT be mistaken for a
 *  context-property read (the whole reason this list exists: proven by the pre-existing
 *  `globalThis.c` false-positive test below). Not exhaustive — anything NOT in this list and not
 *  otherwise known (prop/store/ctx/ref/helper/import) is treated as a possible QObject reference,
 *  which is the conservative direction (an extra, inert Connections block) rather than a missed dep. */
const EFFECT_DEPS_BUILTIN_GLOBALS = new Set([
  "Math", "JSON", "console", "Date", "Object", "Array", "String", "Number", "Boolean", "RegExp",
  "Promise", "Error", "TypeError", "RangeError", "SyntaxError", "Symbol", "Map", "Set", "WeakMap",
  "WeakSet", "Reflect", "Proxy", "globalThis", "window", "document", "process", "undefined",
  "NaN", "Infinity", "parseInt", "parseFloat", "isNaN", "isFinite", "encodeURIComponent",
  "decodeURIComponent", "setTimeout", "clearTimeout", "setInterval", "clearInterval", "Qt",
  "fetch", "localStorage", "structuredClone",
]);

/** Extra, caller-supplied name resolution context for `effectDeps` — everything component.ts
 *  already knows about THIS component's names, needed to route a read to the right Connections
 *  target instead of mistaking it for (or missing) a QObject reference. */
export interface EffectDepsContext {
  /** The props param name (`function C(props)`) — `props.x` reads route to the self property `x`
   *  (props are flattened onto the component root — see `cellRef`/analyzeProps). */
  propsParam?: string | null;
  /** mergeProps/splitProps aliases — `alias.x` reads the same way as `props.x`. */
  propAliases?: Set<string>;
  /** useContext local → ctx name (component.ts's `ctxBindings`). `c.member` routes to the
   *  `__ctx_<ctx>` provider instance's `member` property. */
  ctxBindings?: Record<string, string>;
  /** Per-ctx member shape — a "fn" member (e.g. `c.increment`) is an action, not a reactive read;
   *  only "accessor" (or unknown) members are tracked. */
  ctxValueShape?: Record<string, Record<string, "accessor" | "fn">>;
  /** ref variable name → the QML id of the element it was bound to (component.ts's `refLocals`).
   *  `x.someProp` (not a method CALL on `x`) routes to that id's `someProp`. */
  refLocals?: Record<string, string>;
  /** Names that are known NOT to be QObject/context references — local/module helper names,
   *  imported JS module bindings, module-level consts, context/component/foreign-qml tag names,
   *  mutable locals — so a member read on one of them is never mistaken for a context property. */
  knownNonReactive?: Set<string>;
  /** This component's own local helper functions (`analyzeHelpers(fn)`), so a call to one of them
   *  inside the effect body is followed into its body for further reads — same closure-over-
   *  component-state reasoning Solid itself has for a plain function called from an effect. */
  helpers?: Map<string, HelperInfo>;
}

/** Every reactive read an effect body performs — used to wire `Connections` dep-tracking so the
 *  effect re-runs when any of them changes. Covers, in order of how they're resolved:
 *   - Solid signals/memos/stores read bare (`a` from `a()`, or the store identifier itself,
 *     e.g. `store.field` — a store is reassign-to-rebind, so the dep is the WHOLE object, same as
 *     the existing signal/memo case) — table-driven, unchanged logic from before this function grew.
 *   - `props.x` / a mergeProps-alias `.x` — routes to the self property `x` (one level only,
 *     matching how `cellRef` resolves it at emit time).
 *   - `c.member` where `c` is a useContext consumer binding — routes to `__ctx_<ctx>.member`,
 *     skipped when the member is a known action ("fn" in ctxValueShape).
 *   - `x.prop` where `x` is a `ref={x}` variable and the read is NOT itself a method call
 *     (`x.focus()`) — routes to the element's QML id.
 *   - any OTHER non-computed member chain rooted in an identifier that isn't already covered above
 *     (a table name, a prop/ctx/ref name) and isn't a known non-reactive name or JS/QML builtin
 *     global — a context property injected from C++ (`office.spellReady`), an imported singleton,
 *     or anything else QML's bare-name scope chain resolves. Skipped when the read is itself the
 *     callee of a call (`office.recheckAll()` is an action, not a property read). A chain of length
 *     > 1 (`a.b.c`) emits a dep at EVERY prefix level (`a.b` → `c`, AND `a` → `b`) — deliberate
 *     static OVER-tracking (Solid tracks only the branch actually taken at runtime; we can't, short
 *     of re-deriving Solid's own dependency graph inside QML) rather than under-tracking: it's safe
 *     (an extra Connections block that may just never fire) and it also catches the case where the
 *     INTERMEDIATE object (`a.b`) itself gets swapped for a different one.
 *   - a call to a local helper (component.ts's `helpers`/`analyzeHelpers`) is followed into the
 *     helper's own body (one level of indirection, cycle-guarded) — the helper closes over the same
 *     component state, so a read inside it is exactly as live as one written inline.
 *  NOT analyzable (documented, not attempted): a module-level helper function's body (a different,
 *  shared lexical scope — see component.ts's moduleFnDecls comment), a computed member (`obj[x]`),
 *  or a chain whose root isn't a plain identifier (`f().x`). */
export function effectDeps(body: t.Node, table: SymbolTable, extra: EffectDepsContext = {}): EffectDep[] {
  const { propsParam, propAliases, ctxBindings, ctxValueShape, refLocals, knownNonReactive, helpers } = extra;
  const deps = new Map<string, EffectDep>();
  const add = (target: string, prop: string) => deps.set(`${target}\u0000${prop}`, { target, prop });
  const visitedHelpers = new Set<string>();
  // Names LEXICALLY SHADOWED while scanning a node (the effect body itself, OR a called helper's
  // body): every const/let/var/function/arrow-or-function-expression-param it declares, at ANY
  // depth — real JS shadowing, so a read of one of these refers to the LOCAL, never an outer
  // component prop/ctx/global of the same name. Pushed before scanning a node, popped after (stack
  // discipline — see `scan` below). Proven necessary by TWO real-world shapes:
  //  - a helper's own param: `onPicked = (p) => { if (p.seq <= lastPick()) … }` (office app) — `p`
  //    is onPicked's own, unrelated to any outer `p`.
  //  - a LOCAL declared inside the effect body itself: `var n = JSON.parse(mail.noticeOpen); …
  //    n.category` (solid-mail), or an inline callback param: `accounts.map(function(a) {
  //    return a.id })` — without this, `n`/`a` were wrongly emitted as dep targets (there usually
  //    isn't a top-level `n`/`a` — a silently-dead Connections block at best, a wrong one at worst
  //    if the component happens to also have an unrelated same-named global).
  const shadowed = new Set<string>();

  const scan = (node: t.Node, ownParams: string[] = []): void => {
    const stmts: t.Statement[] = t.isBlockStatement(node)
      ? node.body
      : [t.expressionStatement(node as t.Expression)];
    const newlyShadowed: string[] = [];
    for (const n of localNamesIn(stmts, ownParams)) {
      if (!shadowed.has(n)) { shadowed.add(n); newlyShadowed.push(n); }
    }
    traverse(t.file(t.program(stmts)), {
      Identifier(path) {
        // Skip the property-key position of `obj.a` — `a` there is a plain member access, not a
        // reactive read of a same-named signal/memo. (Babel's Identifier visitor fires on both the
        // object and the non-computed property.)
        if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
        const name = path.node.name;
        if (shadowed.has(name)) return; // a helper-local shadows any same-named component state
        const sym = table.get(name);
        if (sym && (sym.kind === "signal" || sym.kind === "memo" || sym.kind === "store")) add("__self", name);
        // A selector call `isSel(k)` reads its source.
        if (sym?.kind === "selector" && !visitedHelpers.has(`selector:${name}`)) {
          visitedHelpers.add(`selector:${name}`);
          scan(onDepsCall(sym.source));
        }
      },
      MemberExpression(path) {
        const { object, property, computed } = path.node;
        if (computed || !t.isIdentifier(property)) return;
        const prop = property.name;
        const calledAsFn = t.isCallExpression(path.parent) && path.parent.callee === path.node;

        if (t.isIdentifier(object)) {
          const objName = object.name;
          if (shadowed.has(objName)) return; // helper-local object — not a component/context reference

          // props.x / alias.x (one level — matches cellRef's resolution). Skip a CALL
          // (`props.onClick()`) — that's an action/callback prop, not a reactive value read.
          if (objName === propsParam || propAliases?.has(objName)) { if (!calledAsFn) add("__self", prop); return; }

          // useContext member — skip pure-action members.
          if (ctxBindings && objName in ctxBindings) {
            const ctxName = ctxBindings[objName];
            if (ctxValueShape?.[ctxName]?.[prop] !== "fn") add(`__ctx_${safeName(ctxName)}`, prop);
            return;
          }

          // ref variable — skip a method CALL on the element (x.focus()), not a property read.
          if (refLocals && objName in refLocals) {
            if (!calledAsFn) add(refLocals[objName], prop);
            return;
          }

          // Already covered by the Identifier visitor above (whole-object dep) — don't also add a
          // (wrong) member-level target for it.
          if (table.has(objName)) return;
          if (knownNonReactive?.has(objName)) return;
          if (EFFECT_DEPS_BUILTIN_GLOBALS.has(objName)) return;
          if (calledAsFn) return; // a method call, not a property read (e.g. office.recheckAll())
          add(objName, prop);
          return;
        }

        // A longer chain (`a.b.c`): walk the object side so a PREFIX member expression (`a.b`) is
        // also considered here (in addition to being visited on its own by this same visitor,
        // which emits the `a` → `b` dep) — this call only emits the DEEPEST level (`a.b` → `c`);
        // the shallower ones come from Babel visiting each nested MemberExpression independently.
        if (t.isMemberExpression(object) && !object.computed && t.isIdentifier(object.property)) {
          let root: t.Node = object;
          while (t.isMemberExpression(root) && !root.computed && t.isIdentifier(root.property)) root = root.object;
          if (!t.isIdentifier(root)) return;
          const rootName = root.name;
          if (shadowed.has(rootName)) return;
          if (propsParam && rootName === propsParam) return;
          if (propAliases?.has(rootName)) return;
          if (ctxBindings && rootName in ctxBindings) return;
          if (refLocals && rootName in refLocals) return;
          if (table.has(rootName)) return;
          if (knownNonReactive?.has(rootName)) return;
          if (EFFECT_DEPS_BUILTIN_GLOBALS.has(rootName)) return;
          if (calledAsFn) return;
          // Reconstruct the dotted prefix text (`a.b`) — every segment here is a plain, non-computed
          // identifier property, so the source text IS just the dotted join.
          const segs: string[] = [];
          let n: t.Node = object;
          while (t.isMemberExpression(n) && !n.computed && t.isIdentifier(n.property)) { segs.unshift(n.property.name); n = n.object; }
          add([rootName, ...segs].join("."), prop);
        }
      },
      CallExpression(path) {
        const { callee } = path.node;
        // untrack(fn): its reads are NOT dependencies (Solid's untracked read).
        if (t.isIdentifier(callee, { name: "untrack" }) && !shadowed.has("untrack")) { path.skip(); return; }
        if (!t.isIdentifier(callee) || !helpers?.has(callee.name)) return;
        if (visitedHelpers.has(callee.name)) return; // cycle guard (mutually/self-recursive helpers)
        visitedHelpers.add(callee.name);
        const info = helpers.get(callee.name)!;
        scan(info.body, info.params); // `scan` itself shadows the helper's own params/locals
      },
    });
    for (const n of newlyShadowed) shadowed.delete(n); // pop — see the push above (stack discipline)
  };

  scan(body);
  return [...deps.values()];
}

/** Every name a node (an effect body, OR a called helper's body) binds locally — `ownParams`
 *  (a helper's own params; empty for the effect body itself) plus any const/let/var, function
 *  declaration, or nested function/arrow param declared ANYWHERE inside it (any depth) — used by
 *  effectDeps' `scan` to shadow them for the duration of scanning that node (see its own comment
 *  on `shadowed`). Destructuring patterns are NOT walked (documented gap: a destructured param/
 *  local could still be mistaken for a global if read as `x.y` — rare in practice, since a
 *  destructured binding is usually read bare, not member-accessed). */
function localNamesIn(stmts: t.Statement[], ownParams: string[]): Set<string> {
  const names = new Set<string>(ownParams);
  traverse(t.file(t.program(stmts)), {
    VariableDeclarator(path) {
      if (t.isIdentifier(path.node.id)) names.add(path.node.id.name);
    },
    FunctionDeclaration(path) {
      if (path.node.id) names.add(path.node.id.name);
      for (const p of path.node.params) if (t.isIdentifier(p)) names.add(p.name);
    },
    ArrowFunctionExpression(path) {
      for (const p of path.node.params) if (t.isIdentifier(p)) names.add(p.name);
    },
    FunctionExpression(path) {
      for (const p of path.node.params) if (t.isIdentifier(p)) names.add(p.name);
    },
  });
  return names;
}

/** A resource declared in a component: `const [r] = createResource(source, fetcher)`. The loader is
 *  emitted in Component.onCompleted (set r/_loading/_error), reloaded via Connections on the source
 *  signal. The module-level fetcher fn is inlined (no sidecar). */
export interface ResourceInfo { name: string; source: string; fetcher: string; }
export function analyzeResources(fn: t.Function): ResourceInfo[] {
  const out: ResourceInfo[] = [];
  if (!t.isBlockStatement(fn.body)) return out;
  for (const s of fn.body.body) {
    if (!t.isVariableDeclaration(s)) continue;
    for (const d of s.declarations) {
      if (!t.isArrayPattern(d.id) || !d.init || !t.isCallExpression(d.init)
          || !t.isIdentifier(d.init.callee, { name: "createResource" })) continue;
      const r = d.id.elements[0];
      const [src, fetch] = d.init.arguments;
      if (!t.isIdentifier(r) || !t.isIdentifier(src) || !t.isIdentifier(fetch))
        throw new Error("createResource supports `const [r] = createResource(<signal>, <fetcherIdent>)` only");
      out.push({ name: r.name, source: src.name, fetcher: fetch.name });
    }
  }
  return out;
}

/** The module-level declaration nodes the inlined fetcher needs: the fetcher itself plus the
 *  transitive closure of module-level function/const declarations it references (e.g. the
 *  `_async`/`_await` helpers babel-plugin-transform-async-to-promises injects). Returned in source
 *  order so dependencies that are referenced before definition still resolve (V4 hoists fn decls). */
export type ModuleDecl = t.FunctionDeclaration | t.VariableDeclaration;
export function collectFetcherDeps(file: t.File, fetcher: string): ModuleDecl[] {
  // Index module-level declarations by bound name.
  const byName = new Map<string, ModuleDecl>();
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node) && node.id) byName.set(node.id.name, node);
    else if (t.isVariableDeclaration(node))
      for (const d of node.declarations) if (t.isIdentifier(d.id)) byName.set(d.id.name, node);
  }
  // Reachability from the fetcher: collect referenced module-level names.
  const wanted = new Set<string>();
  const stack = [fetcher];
  while (stack.length) {
    const name = stack.pop()!;
    if (wanted.has(name)) continue;
    const decl = byName.get(name);
    if (!decl) continue;
    wanted.add(name);
    traverse(t.file(t.program([decl])), {
      Identifier(path) {
        if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
        const n = path.node.name;
        if (byName.has(n) && !wanted.has(n)) stack.push(n);
      },
    });
  }
  // Emit in source order (so `_await` declared first stays first), deduped.
  const out: ModuleDecl[] = [];
  const seen = new Set<ModuleDecl>();
  for (const node of file.program.body) {
    if (t.isFunctionDeclaration(node) && node.id && wanted.has(node.id.name) && !seen.has(node)) { out.push(node); seen.add(node); }
    else if (t.isVariableDeclaration(node) && node.declarations.some((d) => t.isIdentifier(d.id) && wanted.has(d.id.name)) && !seen.has(node)) { out.push(node); seen.add(node); }
  }
  return out;
}

/** A top-level `let`/`var` declaration with a literal or simple expression init that is MUTATED
 *  (assigned to via `x = ...`, `x += ...`, `x++`, etc.) somewhere in the function body.
 *
 *  These must live on the QML instance (`property var x: <init>`) because handler closures and
 *  helper functions that run after `Component.onCompleted` must share the same mutable slot.
 *  A `var x = <init>` inside onCompleted is local to that closure and is NOT visible to later
 *  handlers — so persisting it as a property is required for correctness.
 *
 *  Returns a map: name → initializer expression. Only `let`/`var` are eligible; `const` is
 *  immutable by definition. The init must be a literal (number, string, bool, null) or an
 *  ArrayExpression/ObjectExpression (simple structural literal). */
export function analyzeMutableLocals(fn: t.Function): Map<string, t.Expression> {
  const out = new Map<string, t.Expression>();
  if (!t.isBlockStatement(fn.body)) return out;

  // Step 1: collect candidate `let`/`var` declarations at the function body's top level with
  // a literal-ish init. Skip `const` (immutable) and reactive declarations.
  const candidates = new Map<string, t.Expression>(); // name → init expression
  for (const s of fn.body.body) {
    if (!t.isVariableDeclaration(s) || s.kind === "const") continue;
    for (const d of s.declarations) {
      if (!t.isIdentifier(d.id) || !d.init) continue;
      if (isReactiveDecl(d)) continue; // handled by analyzeSignals
      if (isSimpleLiteralInit(d.init)) candidates.set(d.id.name, d.init);
    }
  }
  if (candidates.size === 0) return out;

  // Step 2: scan the entire body for write references (assignments, update expressions).
  // Any candidate that is assigned-to anywhere becomes a mutable local → property var.
  const mutated = new Set<string>();
  traverse(
    t.file(t.program(fn.body.body)),
    {
      AssignmentExpression(path) {
        const { left } = path.node;
        if (t.isIdentifier(left) && candidates.has(left.name)) mutated.add(left.name);
      },
      UpdateExpression(path) {
        const { argument } = path.node;
        if (t.isIdentifier(argument) && candidates.has(argument.name)) mutated.add(argument.name);
      },
    },
  );

  for (const [name, init] of candidates) {
    if (mutated.has(name)) out.set(name, init);
  }
  return out;
}

/** Returns true if the expression is a simple literal value (number, string, bool, null) or an
 *  empty array/object literal — suitable as a QML property initializer without evaluation risk. */
function isSimpleLiteralInit(node: t.Expression): boolean {
  return t.isNumericLiteral(node) || t.isStringLiteral(node) || t.isBooleanLiteral(node) ||
    t.isNullLiteral(node) || t.isArrayExpression(node) || t.isObjectExpression(node);
}

/** A declarator that creates a reactive cell (so it is owned by analyzeSignals, not setup). */
function isReactiveDecl(d: t.VariableDeclarator): boolean {
  const init = d.init;
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee)
      && ["createSignal", "createMemo", "createResource", "createStore", "createSelector"].includes(init.callee.name)) return true;
  // a plain arrow assigned to a name = a derived accessor (analyzeSignals records it)
  if (init && t.isArrowFunctionExpression(init) && !t.isJSXElement(init.body) && !t.isJSXFragment(init.body)) return true;
  // prop-helper aliases (mergeProps/splitProps) — modelled by analyzeProps, not setup
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee)
      && ["mergeProps", "splitProps"].includes(init.callee.name)) return true;
  // useContext binding — modelled by analyzeUseContext, resolved through __ctx_<ctx>, not setup
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "useContext" })) return true;
  // `const c = children(() => props.children)` — a children slot accessor (analyzeChildrenAccessors)
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "children" })) return true;
  return false;
}

/** A component's props: the param name, the prop names it reads (via props.X or a mergeProps/
 *  splitProps alias.X), default values (from mergeProps' defaults object), and the alias names
 *  (variables bound to mergeProps/splitProps that forward to props). */
export function analyzeProps(fn: t.Function): { param: string | null; used: string[]; defaults: Record<string, t.Expression>; aliases: string[]; rests: Record<string, string[]> } {
  const param = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const used = new Set<string>();
  const defaults: Record<string, t.Expression> = {};
  const aliases = new Set<string>();
  /** splitProps rest aliases: name → the keys split OFF (absent from it). */
  const rests: Record<string, string[]> = {};
  if (fn.params[0] && !param)
    throw new Error("a component's props must be one parameter (`function C(props)`): destructuring it (`({ a, b })`) reads each prop once, untracked — not supported natively");
  if (!param) return { param: null, used: [], defaults, aliases: [], rests };

  const body = t.file(t.program(fn.body.type === "BlockStatement" ? fn.body.body : [t.expressionStatement(fn.body)]));
  // First pass: find alias declarations + mergeProps defaults.
  for (const stmt of (fn.body.type === "BlockStatement" ? fn.body.body : [])) {
    if (!t.isVariableDeclaration(stmt)) continue;
    for (const d of stmt.declarations) {
      const init = d.init;
      if (!init || !t.isCallExpression(init) || !t.isIdentifier(init.callee)) continue;
      if (init.callee.name === "mergeProps") {
        if (t.isIdentifier(d.id)) aliases.add(d.id.name);
        // An object AFTER props overrides it (Solid: the last source with a defined value wins);
        // only the defaults form `mergeProps({ … }, props)` maps onto overridable properties.
        const at = init.arguments.findIndex((a) => t.isIdentifier(a, { name: param }));
        if (at >= 0 && init.arguments.slice(at + 1).some((a) => t.isObjectExpression(a)))
          throw new Error("mergeProps(props, { … }): an object after props OVERRIDES the passed props — not supported natively; put the defaults first: mergeProps({ … }, props)");
        // NB: every object-literal arg is treated as defaults (prop init values). mergeProps used
        // for last-wins OVERRIDE (object after props) is NOT modelled — out of scope for now.
        for (const arg of init.arguments) // the object-literal arg(s) carry defaults
          if (t.isObjectExpression(arg))
            for (const p of arg.properties)
              if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) defaults[p.key.name] = p.value;
      } else if (init.callee.name === "splitProps") {
        // const [local, rest] = splitProps(props, [...]) — `local` (first element) aliases props.
        if (t.isArrayPattern(d.id) && d.id.elements[0] && t.isIdentifier(d.id.elements[0])) aliases.add((d.id.elements[0] as t.Identifier).name);
        // `rest` (second element): the other props — read through it like props, spread as props
        // minus the split keys.
        const rest = t.isArrayPattern(d.id) ? d.id.elements[1] : null;
        if (rest && t.isIdentifier(rest)) {
          const keys = init.arguments[1];
          if (!keys || !t.isArrayExpression(keys) || !keys.elements.every((e) => t.isStringLiteral(e)))
            throw new Error("splitProps(props, keys): keys must be an array of string literals");
          aliases.add(rest.name);
          rests[rest.name] = keys.elements.map((e) => (e as t.StringLiteral).value);
        }
      }
    }
  }
  // Second pass: props read via props.X or alias.X.
  traverse(body, {
    MemberExpression(path) {
      const { object, property, computed } = path.node;
      if (computed || !t.isIdentifier(object) || !t.isIdentifier(property)) return;
      if (object.name === param || aliases.has(object.name)) used.add(property.name);
    },
  });
  return { param, used: [...used], defaults, aliases: [...aliases], rests };
}

/** `const c = children(() => props.children)` at a component's top level: `c` is a children slot
 *  accessor — `{c()}` directly inside the root element slots the children exactly like
 *  `{props.children}`. Resolving or inspecting the children (`c.toArray()`, `c()` used as a value)
 *  has no native equivalent (the children are QML objects mounted into the root) and fails loud. */
export function analyzeChildrenAccessors(fn: t.Function, render: t.CallExpression, propsParam: string | null, propAliases: Set<string>): Set<string> {
  const out = new Set<string>();
  if (!t.isBlockStatement(fn.body)) return out;
  const decls = new Set<t.Node>();
  for (const st of fn.body.body) {
    if (!t.isVariableDeclaration(st)) continue;
    for (const d of st.declarations) {
      if (!d.init || !t.isCallExpression(d.init) || !t.isIdentifier(d.init.callee, { name: "children" })) continue;
      const arg = d.init.arguments[0];
      const body = arg && (t.isArrowFunctionExpression(arg) || t.isFunctionExpression(arg)) && arg.params.length === 0
        ? (t.isBlockStatement(arg.body) ? (arg.body.body.length === 1 && t.isReturnStatement(arg.body.body[0]) ? arg.body.body[0].argument : null) : arg.body)
        : null;
      const ok = !!body && t.isMemberExpression(body) && !body.computed && t.isIdentifier(body.property, { name: "children" })
        && t.isIdentifier(body.object) && (body.object.name === propsParam || propAliases.has(body.object.name));
      if (!ok || !t.isIdentifier(d.id))
        throw new Error("children(): only `const c = children(() => props.children)` is supported natively");
      out.add(d.id.name);
      decls.add(d.id);
    }
  }
  if (!out.size) return out;
  // The only allowed use: `{c()}` as a direct child of the root element.
  const rootSlots = new Set<t.Node>(hParts(render).children.filter((c) => t.isCallExpression(c) && c.arguments.length === 0
    && t.isIdentifier(c.callee) && out.has(c.callee.name)).map((c) => (c as t.CallExpression).callee));
  traverse(t.file(t.program(fn.body.body)), {
    Identifier(path) {
      if (!out.has(path.node.name) || decls.has(path.node) || rootSlots.has(path.node)) return;
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      if (t.isObjectProperty(path.parent) && !path.parent.computed && path.parent.key === path.node) return;
      throw new Error(`children(): \`${path.node.name}\` can only be rendered as {${path.node.name}()} directly inside the component's root element — resolving or inspecting the children (${path.node.name}.toArray(), ${path.node.name}() as a value, inside <Show>/<For>) is not supported natively`);
    },
  });
  return out;
}

/** Top-level helper functions in a component body: `function f(params) { body }` (FunctionDeclaration)
 *  and `const f = (params) => body` (block-bodied arrow) or `const f = (params) => expr` (expression
 *  arrow WITH params). Zero-param expression arrows normally stay as "derived" (analyzeSignals,
 *  inlined at call sites) — EXCEPT a promoted one (referenced by value elsewhere, not just called;
 *  see `hasNonCallReference`), which is picked up here instead. Block-bodied arrows and param-having
 *  arrows become helpers — they are emitted as QML object methods so calls to siblings
 *  (`toggle(t.id)`) resolve bare through the object scope.
 *
 *  Excluded: createSignal/Memo/Resource, mergeProps/splitProps, useContext (handled elsewhere).
 *  Returns a map: helper name → { params, body as BlockStatement }. */
export interface HelperInfo {
  /** Every name the params bind (a destructured param binds each name in its pattern). */
  params: string[];
  /** The param nodes themselves — identifiers or destructuring patterns — for the emitted signature. */
  paramNodes: t.Function["params"];
  body: t.BlockStatement;
}
function helperInfo(params: t.Function["params"], body: t.BlockStatement): HelperInfo {
  return { params: params.flatMap((p) => Object.keys(t.getBindingIdentifiers(p))), paramNodes: params, body };
}
export function analyzeHelpers(fn: t.Function): Map<string, HelperInfo> {
  const out = new Map<string, HelperInfo>();
  if (!t.isBlockStatement(fn.body)) return out;
  for (const s of fn.body.body) {
    // function foo(params) { body } → helper
    if (t.isFunctionDeclaration(s) && s.id) {
      out.set(s.id.name, helperInfo(s.params, s.body));
      continue;
    }
    if (!t.isVariableDeclaration(s) || s.kind !== "const") continue;
    for (const d of s.declarations) {
      if (!t.isIdentifier(d.id) || !d.init) continue;
      const init = d.init;
      // Skip non-function declarations (createSignal, createMemo, mergeProps, useContext, etc.)
      if (t.isCallExpression(init)) continue; // handled by analyzeSignals / analyzeProps / etc.
      if (t.isObjectExpression(init)) continue; // structural (provider value local, etc.)
      if (t.isArrowFunctionExpression(init) && !t.isJSXElement(init.body) && !t.isJSXFragment(init.body)) {
        // Zero-param expression arrows normally stay "derived" accessors (analyzeSignals, inlined
        // at call sites) — UNLESS something elsewhere in the component references it by VALUE
        // (not just calling it): `register(f)`, `onClick={f}`, `setTimeout(f, 0)`, `arr.map(f)`,
        // storing it (`{cb: f}`), returning it, … There is no call site such a reference could
        // inline INTO, so it is promoted to a real method here instead — the EXACT same predicate
        // analyzeSignals uses to exclude it from the derived table, so the two never disagree
        // about which path a name takes (see hasNonCallReference's own doc comment).
        if (init.params.length === 0 && !t.isBlockStatement(init.body)) {
          if (!hasNonCallReference(fn, d.id.name, d.id)) continue; // stays inlined — not a helper
          out.set(d.id.name, helperInfo([], exprArrowToHelperBody(init.body)));
          continue;
        }
        // Param-having or block-bodied arrows → helpers (QML object methods). An expression body
        // IS the return value — wrapping it as a bare statement silently made every such helper
        // return undefined.
        out.set(d.id.name, helperInfo(init.params, exprArrowToHelperBody(init.body)));
      } else if (t.isFunctionExpression(init)) {
        out.set(d.id.name, helperInfo(init.params, init.body));
      }
    }
  }
  return out;
}

/** Babel's object-spread helper for JSX props (`_extends`, `_extends2`…). */
export function isSpreadHelperName(name: string): boolean {
  return /^_extends\d*$/.test(name);
}

/** A component instance's props argument as an ordered list of sources: object literals and spread
 *  expressions (`<C a={1} {...o} b={2}>` → `{a}`, `o`, `{b}`). */
export type PropSource = { props: t.ObjectProperty[] } | { spread: t.Expression };
export function propSources(propsArg: t.Node | undefined): PropSource[] {
  if (!propsArg || t.isNullLiteral(propsArg)) return [];
  const one = (n: t.Node): PropSource => {
    if (t.isObjectExpression(n)) {
      if (n.properties.some((p) => !t.isObjectProperty(p))) throw new Error("component props: only `key={value}` and `{...spread}` attributes are supported");
      return { props: n.properties as t.ObjectProperty[] };
    }
    if (t.isExpression(n)) return { spread: n };
    throw new Error(`component props: unsupported ${n.type}`);
  };
  if (t.isCallExpression(propsArg) && ((t.isIdentifier(propsArg.callee) && isSpreadHelperName(propsArg.callee.name))
      || (t.isMemberExpression(propsArg.callee) && t.isIdentifier(propsArg.callee.object, { name: "Object" }) && t.isIdentifier(propsArg.callee.property, { name: "assign" }))))
    return propsArg.arguments.map(one).filter((src) => !("props" in src) || src.props.length > 0);
  return [one(propsArg)];
}

/** True if a function-like body IS a component (returns an `h(...)` call) — the same shape
 *  `findComponents` (index.ts) looks for. A module-level declaration with this shape is a
 *  component, not a plain helper, and must never be pulled into another component as a method. */
function looksLikeComponentBody(body: t.BlockStatement | t.Expression): boolean {
  if (isHCall(body)) return true; // expression-bodied arrow whose body IS the h() call
  if (!t.isBlockStatement(body)) return false;
  return body.body.some((s) => t.isReturnStatement(s) && isHCall(s.argument));
}

/** Module-level helper functions: `function f(params) { body }` (FunctionDeclaration) and
 *  `const f = (params) => body` / `const f = function(params) { body }` at the TOP of the file —
 *  the module-scope analog of `analyzeHelpers`. Excludes anything that is itself a component
 *  (returns an `h(...)` call — `findComponents` already owns those). Unlike `analyzeHelpers`,
 *  a zero-param expression arrow IS included here: there is no module-level "derived accessor"
 *  mechanism to hand it to, so `const f = () => expr` used as `f()` needs to become a method too.
 *
 *  Returns every module-level function-shaped binding, regardless of whether any component uses
 *  it — callers select the subset actually referenced (collectModuleFunctionClosure) so dead
 *  helpers are never emitted. */
export function analyzeModuleFunctions(file: t.File): Map<string, HelperInfo> {
  const out = new Map<string, HelperInfo>();
  for (const node of file.program.body) {
    // Babel's JSX spread helper (`h(C, _extends({…}, o))`): consumed by the instance emitter.
    if (t.isFunctionDeclaration(node) && node.id && isSpreadHelperName(node.id.name)) continue;
    if (t.isFunctionDeclaration(node) && node.id && t.isBlockStatement(node.body)) {
      if (looksLikeComponentBody(node.body)) continue;
      out.set(node.id.name, helperInfo(node.params, node.body));
      continue;
    }
    if (!t.isVariableDeclaration(node) || node.kind !== "const") continue;
    for (const d of node.declarations) {
      if (!t.isIdentifier(d.id) || !d.init) continue;
      const init = d.init;
      if (t.isArrowFunctionExpression(init)) {
        if (looksLikeComponentBody(init.body)) continue;
        const body: t.BlockStatement = t.isBlockStatement(init.body)
          ? init.body
          : t.blockStatement([t.returnStatement(init.body as t.Expression)]);
        out.set(d.id.name, helperInfo(init.params, body));
      } else if (t.isFunctionExpression(init)) {
        if (looksLikeComponentBody(init.body)) continue;
        out.set(d.id.name, helperInfo(init.params, init.body));
      }
    }
  }
  return out;
}

/** Every top-level `let`/`var` declared name in the module — mutable module state. A module-level
 *  helper function closing over one of these cannot be supported correctly: the function is
 *  emitted as a METHOD COPIED into every component that references it (each component is its own
 *  QML type/instance), so there is no single shared slot a mutation could land in — two components
 *  calling the "same" function would silently mutate independent copies instead of shared state.
 *  `collectModuleFunctionClosure` fails loud (transpile-time error) rather than emit that silently
 *  wrong behavior. */
export function moduleLevelMutableNames(file: t.File): Set<string> {
  const out = new Set<string>();
  for (const node of file.program.body) {
    if (!t.isVariableDeclaration(node) || node.kind === "const") continue;
    for (const d of node.declarations) if (t.isIdentifier(d.id)) out.add(d.id.name);
  }
  return out;
}

/** Every Identifier name referenced inside a helper body (own declaration keys excluded by the
 *  caller via `paramNames`, since a function's own params shadow same-named outer bindings).
 *  Over-approximates like `collectIdentifierNames` (component.ts) — deeper shadowing (a nested
 *  `const`/param with the same name) is not tracked; module helper names are distinctive in
 *  practice, same tradeoff already accepted for module consts. */
function identifiersInBody(body: t.BlockStatement): Set<string> {
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
  walk(body);
  return names;
}

/** The transitive closure of module-level helper functions a component needs, starting from the
 *  names it directly references (`seedNames` — already filtered by the caller for shadowing: a
 *  name bound locally in the component wins and is never pulled from the module). A selected
 *  helper may itself call another module helper or read a module const — both are discovered by
 *  walking the selected helper's own body (excluding ITS OWN params, which shadow same-named
 *  module decls inside that body — real JS lexical scoping, one function at a time).
 *
 *  Throws if a selected helper's body references module-level mutable `let`/`var` state (see
 *  `moduleLevelMutableNames`) — fail loud at transpile time rather than silently emit a function
 *  whose semantics (shared mutable state across components) this per-component-copy model can't
 *  preserve.
 *
 *  Returns the selected helpers (name → body/params, ready to emit as QML methods) plus every
 *  identifier name referenced by their bodies (minus each one's own params) — the caller unions
 *  this into its module-CONST reference check, so a const only reached transitively through a
 *  helper (never named directly in the component) is still surfaced. */
export function collectModuleFunctionClosure(
  seedNames: Iterable<string>,
  moduleFunctions: Map<string, HelperInfo>,
  moduleMutableNames: Set<string>,
): { selected: Map<string, HelperInfo>; referencedNames: Set<string> } {
  const selected = new Map<string, HelperInfo>();
  const referencedNames = new Set<string>();
  const stack = [...seedNames];
  while (stack.length) {
    const name = stack.pop()!;
    if (selected.has(name)) continue;
    const info = moduleFunctions.get(name);
    if (!info) continue; // not a module function (a module const, or an unrelated identifier)
    selected.set(name, info);
    const paramNames = new Set(info.params);
    for (const ref of identifiersInBody(info.body)) {
      if (paramNames.has(ref)) continue; // the helper's own param shadows a same-named module decl
      referencedNames.add(ref);
      if (moduleMutableNames.has(ref)) {
        throw new Error(
          `module-level function "${name}" closes over mutable module state "${ref}" (a top-level ` +
          `\`let\`/\`var\`) — not supported: the function is copied into every component that calls ` +
          `it, so there is no single shared slot for the mutation. Make "${ref}" a \`const\`, or move ` +
          `both "${ref}" and "${name}" inside the component(s) that need them.`,
        );
      }
      if (moduleFunctions.has(ref)) stack.push(ref);
    }
  }
  return { selected, referencedNames };
}
