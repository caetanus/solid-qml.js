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

/** Top-level immutable locals whose values are read by the render tree.
 *
 *  They must be QML properties, not `var` declarations inside Component.onCompleted, because render
 *  bindings are evaluated outside that lifecycle closure. This covers code like:
 *    const encoded = Base64.encode("solid-qml");
 *    return <text>{encoded}</text>
 */
export function analyzeRenderLocals(fn: t.Function, render: t.CallExpression): Map<string, t.Expression> {
  const candidates = new Map<string, t.Expression>();
  if (!t.isBlockStatement(fn.body)) return candidates;

  for (const s of fn.body.body) {
    if (t.isReturnStatement(s)) break;
    if (!t.isVariableDeclaration(s) || s.kind !== "const") continue;
    if (s.declarations.length !== 1) continue;
    const d = s.declarations[0];
    if (!t.isIdentifier(d.id) || !d.init || !t.isExpression(d.init)) continue;
    if (isReactiveDecl(d)) continue;
    if (t.isArrowFunctionExpression(d.init) || t.isFunctionExpression(d.init)) continue;
    candidates.set(d.id.name, d.init);
  }
  if (candidates.size === 0) return candidates;

  const used = new Set<string>();
  traverse(t.file(t.program([t.expressionStatement(render)])), {
    Identifier(path) {
      if (t.isMemberExpression(path.parent) && !path.parent.computed && path.parent.property === path.node) return;
      if (t.isObjectProperty(path.parent) && !path.parent.computed && path.parent.key === path.node) return;
      if (candidates.has(path.node.name)) used.add(path.node.name);
    },
  });

  for (const name of [...candidates.keys()]) if (!used.has(name)) candidates.delete(name);
  return candidates;
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
