import _traverse from "@babel/traverse";
import * as t from "@babel/types";
import { hParts, isHCall } from "../ast/h.ts";

const traverse: typeof _traverse = (_traverse as any).default ?? _traverse;

/** What a name in a component means. */
export type Sym =
  | { kind: "signal"; name: string; setter: string; init: t.Expression | null }
  | { kind: "setter"; signal: string }
  | { kind: "store"; name: string; setter: string; init: t.Expression | null }
  | { kind: "storeSetter"; store: string }
  | { kind: "memo"; name: string; body: t.Expression | t.BlockStatement }
  | { kind: "resource"; name: string; source: string; fetcher: string }
  | { kind: "derived"; name: string; params: string[]; body: t.Expression | t.BlockStatement };

export type SymbolTable = Map<string, Sym>;

/**
 * Scan a component function body for reactive declarations:
 *  - `const [g, s] = createSignal(init)` → a signal (g) + its setter (s)
 *  - `const m = createMemo(() => …)`     → a memo (a readonly derived property)
 *  - `const d = (…) => <expr>`           → a derived accessor (inlined where called, stays reactive)
 */
export function analyzeSignals(fn: t.Function): SymbolTable {
  const table: SymbolTable = new Map();
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
          const arg = init.arguments[0];
          if (arg && (t.isArrowFunctionExpression(arg) || t.isFunctionExpression(arg)))
            table.set(id.name, { kind: "memo", name: id.name, body: arg.body });
          return;
        }

        // Only zero-param, expression-body arrows become derived accessors (inlined at call sites).
        // Param-having or block-bodied arrows are helpers (analyzeHelpers picks them up as methods).
        if (t.isArrowFunctionExpression(init) && !t.isJSXElement(init.body) && !t.isJSXFragment(init.body)
            && init.params.length === 0 && !t.isBlockStatement(init.body)) {
          table.set(id.name, { kind: "derived", name: id.name, params: [], body: init.body });
        }
      },
    },
  );
  return table;
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

/** A component's non-render lifecycle logic: imperative setup statements (everything at the body's
 *  top level that is NOT a reactive declaration, an onMount call, a createEffect call, or the render
 *  return) plus the onMount callback bodies (run after setup, per Solid order) and createEffect
 *  bodies (run once after mount, re-run via Connections on each reactive dep they read).
 *
 *  `mutableLocals` (from analyzeMutableLocals) are excluded from setup: they become `property var`
 *  declarations on the instance, so their initializer is already emitted as a property value and
 *  the statement must not also appear in onCompleted. */
export function analyzeSetup(fn: t.Function, mutableLocals?: Set<string>, renderLocals?: Set<string>): { setup: t.Statement[]; onMount: t.Node[]; effects: t.Node[] } {
  const setup: t.Statement[] = [];
  const onMount: t.Node[] = [];
  const effects: t.Node[] = [];
  if (!t.isBlockStatement(fn.body)) return { setup, onMount, effects };
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
      if (cb && (t.isArrowFunctionExpression(cb) || t.isFunctionExpression(cb))) onMount.push(cb.body);
      continue;
    }
    // createEffect(fn) → collect the callback body; do NOT fall through to setup
    if (t.isExpressionStatement(s) && t.isCallExpression(s.expression)
        && t.isIdentifier(s.expression.callee, { name: "createEffect" })) {
      const cb = s.expression.arguments[0];
      if (cb && (t.isArrowFunctionExpression(cb) || t.isFunctionExpression(cb))) effects.push(cb.body);
      continue;
    }
    setup.push(s);
  }
  return { setup, onMount, effects };
}

/** Identifier reads of any name in `names` inside `expr` — skipping a property KEY position
 *  (`obj.x` / `{x: ...}`, non-computed), which is not a value read of a same-named candidate.
 *  Shared by the render-usage scan and the inter-candidate reference scan below. */
function readNamesIn(expr: t.Expression, names: Set<string>): Set<string> {
  const found = new Set<string>();
  traverse(t.file(t.program([t.expressionStatement(expr)])), {
    Identifier(path) {
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      if (t.isObjectProperty(path.parent) && !path.parent.computed && path.parent.key === path.node) return;
      if (names.has(path.node.name)) found.add(path.node.name);
    },
  });
  return found;
}

/** Names of signals/memos/resources read by `expr` (ignoring property-key positions) — used to
 *  detect a plain const/let whose initializer reads reactive state. */
function reactiveReadsIn(expr: t.Expression, table: SymbolTable): string[] {
  const found = new Set<string>();
  traverse(t.file(t.program([t.expressionStatement(expr)])), {
    Identifier(path) {
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      const sym = table.get(path.node.name);
      if (sym && (sym.kind === "signal" || sym.kind === "memo" || sym.kind === "resource")) found.add(path.node.name);
    },
  });
  return [...found];
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
 *  Does NOT check for a signal/memo/resource read in the initializer — see
 *  assertNoLiveRenderLocalReads below, called separately once the caller (component.ts) has
 *  removed its own deliberate exception from the map. */
export function analyzeRenderLocals(fn: t.Function, render: t.CallExpression, mutableLocals?: Set<string>): Map<string, t.Expression> {
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

  // Seed from direct render usage, then close over inter-candidate references (BFS).
  const used = readNamesIn(render, names);
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

/** Guard for the signal-read case, called SEPARATELY (component.ts) from analyzeRenderLocals,
 *  AFTER a context provider's own value-local (`provider?.valueLocal`) has been deleted from the
 *  map — that one is a DELIBERATE exception: `const counter = { count, increment: … }` passed as
 *  `<Ctx.Provider value={counter}>` is EXPECTED to stay live (every descendant's `counter.count()`
 *  must track the signal — that is the entire point of the context value object; proven by the
 *  shipped context golden tests, which read a signal bare inside an object literal assigned to
 *  such a const and rely on it staying reactive). Called on anything STILL in the map:
 *
 *  Solid evaluates a plain const/let ONCE, even if its initializer reads a signal/memo/resource —
 *  unlike createMemo, it does NOT re-run when that cell changes. Emitting it as a QML `readonly
 *  property var` binding would make it silently LIVE instead (QML tracks any property read during a
 *  binding's evaluation, including through nested calls — verified against the real engine:
 *  wrapping the read in an IIFE does not stop it from re-evaluating). There is no snapshot that is
 *  both synchronously available (render needs the value at construction time, before
 *  Component.onCompleted runs — also verified: a plain Component.onCompleted assignment arrives too
 *  late and the binding referencing it already crashed) and untracked. Fail loud instead of
 *  silently turning a const into a memo. */
export function assertNoLiveRenderLocalReads(renderLocals: Map<string, t.Expression>, table: SymbolTable): void {
  for (const [name, init] of renderLocals) {
    const reads = reactiveReadsIn(init, table);
    if (reads.length > 0)
      throw new Error(`const ${name}: its initializer reads reactive state (${reads.join(", ")}) and is read by a render binding — Solid evaluates a plain const/let ONCE, but emitting it as a QML property binding here would make it live (recomputing whenever ${reads[0]} changes) instead. Use createMemo() if ${name} should track ${reads[0]}, or compute it without reading reactive state.`);
  }
}

/** Names of signals/memos read by an effect body — used to wire `Connections` dep-tracking so the
 *  effect re-runs when a reactive cell it reads changes. */
export function effectDeps(body: t.Node, table: SymbolTable): string[] {
  const deps = new Set<string>();
  const stmts: t.Statement[] = t.isBlockStatement(body)
    ? body.body
    : [t.expressionStatement(body as t.Expression)];
  traverse(t.file(t.program(stmts)), {
    Identifier(path) {
      // Skip the property-key position of `obj.a` — `a` there is a plain member access, not a
      // reactive read of a same-named signal/memo. (Babel's Identifier visitor fires on both the
      // object and the non-computed property.)
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      const name = path.node.name;
      const sym = table.get(name);
      if (sym && (sym.kind === "signal" || sym.kind === "memo")) deps.add(name);
    },
  });
  return [...deps];
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
      && ["createSignal", "createMemo", "createResource", "createStore"].includes(init.callee.name)) return true;
  // a plain arrow assigned to a name = a derived accessor (analyzeSignals records it)
  if (init && t.isArrowFunctionExpression(init) && !t.isJSXElement(init.body) && !t.isJSXFragment(init.body)) return true;
  // prop-helper aliases (mergeProps/splitProps) — modelled by analyzeProps, not setup
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee)
      && ["mergeProps", "splitProps"].includes(init.callee.name)) return true;
  // useContext binding — modelled by analyzeUseContext, resolved through __ctx_<ctx>, not setup
  if (init && t.isCallExpression(init) && t.isIdentifier(init.callee, { name: "useContext" })) return true;
  return false;
}

/** A component's props: the param name, the prop names it reads (via props.X or a mergeProps/
 *  splitProps alias.X), default values (from mergeProps' defaults object), and the alias names
 *  (variables bound to mergeProps/splitProps that forward to props). */
export function analyzeProps(fn: t.Function): { param: string | null; used: string[]; defaults: Record<string, t.Expression>; aliases: string[] } {
  const param = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const used = new Set<string>();
  const defaults: Record<string, t.Expression> = {};
  const aliases = new Set<string>();
  if (!param) return { param: null, used: [], defaults, aliases: [] };

  const body = t.file(t.program(fn.body.type === "BlockStatement" ? fn.body.body : [t.expressionStatement(fn.body)]));
  // First pass: find alias declarations + mergeProps defaults.
  for (const stmt of (fn.body.type === "BlockStatement" ? fn.body.body : [])) {
    if (!t.isVariableDeclaration(stmt)) continue;
    for (const d of stmt.declarations) {
      const init = d.init;
      if (!init || !t.isCallExpression(init) || !t.isIdentifier(init.callee)) continue;
      if (init.callee.name === "mergeProps") {
        if (t.isIdentifier(d.id)) aliases.add(d.id.name);
        // NB: every object-literal arg is treated as defaults (prop init values). mergeProps used
        // for last-wins OVERRIDE (object after props) is NOT modelled — out of scope for now.
        for (const arg of init.arguments) // the object-literal arg(s) carry defaults
          if (t.isObjectExpression(arg))
            for (const p of arg.properties)
              if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) defaults[p.key.name] = p.value;
      } else if (init.callee.name === "splitProps") {
        // const [local, rest] = splitProps(props, [...]) — `local` (first element) aliases props.
        if (t.isArrayPattern(d.id) && d.id.elements[0] && t.isIdentifier(d.id.elements[0])) aliases.add((d.id.elements[0] as t.Identifier).name);
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
  return { param, used: [...used], defaults, aliases: [...aliases] };
}

/** Top-level helper functions in a component body: `function f(params) { body }` (FunctionDeclaration)
 *  and `const f = (params) => body` (block-bodied arrow) or `const f = (params) => expr` (expression
 *  arrow WITH params). Zero-param expression arrows stay as "derived" (analyzeSignals). Block-bodied
 *  arrows and param-having arrows become helpers — they are emitted as QML object methods so calls to
 *  siblings (`toggle(t.id)`) resolve bare through the object scope.
 *
 *  Excluded: createSignal/Memo/Resource, mergeProps/splitProps, useContext (handled elsewhere).
 *  Returns a map: helper name → { params, body as BlockStatement }. */
export interface HelperInfo { params: string[]; body: t.BlockStatement }
export function analyzeHelpers(fn: t.Function): Map<string, HelperInfo> {
  const out = new Map<string, HelperInfo>();
  if (!t.isBlockStatement(fn.body)) return out;
  for (const s of fn.body.body) {
    // function foo(params) { body } → helper
    if (t.isFunctionDeclaration(s) && s.id) {
      const params = s.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
      out.set(s.id.name, { params, body: s.body });
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
        // Zero-param expression arrows → "derived" accessors handled by analyzeSignals; skip here.
        if (init.params.length === 0 && !t.isBlockStatement(init.body)) continue;
        // Param-having or block-bodied arrows → helpers (QML object methods). An expression body
        // IS the return value — wrapping it as a bare statement silently made every such helper
        // return undefined.
        const params = init.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
        const body: t.BlockStatement = t.isBlockStatement(init.body)
          ? init.body
          : t.blockStatement([t.returnStatement(init.body as t.Expression)]);
        out.set(d.id.name, { params, body });
      } else if (t.isFunctionExpression(init)) {
        const params = init.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
        out.set(d.id.name, { params, body: init.body });
      }
    }
  }
  return out;
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
    if (t.isFunctionDeclaration(node) && node.id && t.isBlockStatement(node.body)) {
      if (looksLikeComponentBody(node.body)) continue;
      const params = node.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
      out.set(node.id.name, { params, body: node.body });
      continue;
    }
    if (!t.isVariableDeclaration(node) || node.kind !== "const") continue;
    for (const d of node.declarations) {
      if (!t.isIdentifier(d.id) || !d.init) continue;
      const init = d.init;
      if (t.isArrowFunctionExpression(init)) {
        if (looksLikeComponentBody(init.body)) continue;
        const params = init.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
        const body: t.BlockStatement = t.isBlockStatement(init.body)
          ? init.body
          : t.blockStatement([t.returnStatement(init.body as t.Expression)]);
        out.set(d.id.name, { params, body });
      } else if (t.isFunctionExpression(init)) {
        if (looksLikeComponentBody(init.body)) continue;
        const params = init.params.filter((p): p is t.Identifier => t.isIdentifier(p)).map((p) => p.name);
        out.set(d.id.name, { params, body: init.body });
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
