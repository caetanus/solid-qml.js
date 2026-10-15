import * as t from "@babel/types";
import { safeName, memberName } from "../names/safe.ts";
import type { SymbolTable } from "../model/symbols.ts";
import { emitStatements } from "./stmt.ts";

/** Where an expression sits decides how names resolve. */
export type Mode = "binding" | "handler" | "init";

export interface Scope {
  table: SymbolTable;
  mode: Mode;
  /** Arrow/closure params bound to a replacement string (e.g. a setter updater's `prev`). */
  locals?: Record<string, string>;
  propsParam?: string;
  /** Use-name (the tag in h(Tag,…)) → emitted QML type name. */
  components?: Map<string, string>;
  /** Emitted type names that are foreign hand-written `.qml` components (not Css roots): their
   *  instances are wrapped in a Css box so they participate in the CSS layout (escape hatch). */
  foreignQml?: Set<string>;
  // Registered-module foreign types (`import { X } from "qml:Uri"`): typeName → import line the
  // emitting component must carry (routed to usedWidgets.extraImports at instantiation).
  foreignImports?: Map<string, string>;
  /** Root object id to qualify signal WRITES with (setter assignments only). Set when a handler
   *  runs INSIDE a Template control whose own properties (value/angle/position/…) would otherwise
   *  shadow a same-named component signal. Reads stay bare so QML's binding tracker still sees them. */
  selfId?: string;
  /** Collector: ref variable names seen during emitQml (pushed by the container emitter when
   *  ref={x} is encountered). Read by emitComponentType after emitting the render tree to populate
   *  setupScope.locals so onMount bodies resolve the ref to its QML id. */
  refs?: string[];
  /** Collector: refs whose element lives inside a lazily mounted subtree (<Show>). Their QML id is
   *  not reachable from outside the delegate, so the element PUBLISHES itself into a root property
   *  of the same name; emitComponentType declares one `property var _ref_<x>` per entry. */
  lazyRefs?: Set<string>;
  /** Zero-arg "accessor" names whose call resolves to a fixed QML expression — e.g. an <Index>
   *  row accessor `item()` → `modelData`. Distinct from `locals` (which substitutes the bare
   *  identifier) because here the CALL `name()` is what maps. */
  accessors?: Record<string, string>;
  /** Names that alias the props object (mergeProps/splitProps results): `alias.X` resolves to prop X. */
  propAliases?: Set<string>;
  /** Known context names (top-level `createContext`), so `<X.Provider>` tags are recognized. */
  contexts?: Set<string>;
  /** Consumer's `useContext` bindings: local name → context name. `c.member` resolves through the
   *  injected provider instance: `__ctx_<ctx>.member`. */
  ctxBindings?: Record<string, string>;
  /** Per-context value-member shape (from the provider's analysis): member → "accessor" | "fn".
   *  An accessor member read as a zero-arg call (`c.count()`) drops the call (reactive property read). */
  ctxValueShape?: Record<string, Record<string, "accessor" | "fn">>;
  /** Per-instantiated-component (keyed by the render-tree use-name/tag) context role: the ctx it
   *  provides (if a provider), the ctxs it consumes (if a consumer). Drives provider-stack push +
   *  consumer `__ctx_<ctx>` injection in emitInstance. */
  componentMeta?: Map<string, { provides?: string; consumes?: string[] }>;
  /** The active provider stack while emitting under provider instances: ctx → providerInstanceId.
   *  A consumer instance of a stacked ctx gets `__ctx_<ctx>: <id>` injected. */
  providerStack?: Record<string, string>;
  /** Mutable counter for unique provider instance ids (`__ctxprovN`). Shared across one emit pass. */
  ctxProvCounter?: { n: number };
  /** Mutable counter for unique TextInput ids (`__inputN`). Shared across one emit pass. */
  inputCounter?: { n: number };
  /** Mutable counter for unique button MouseArea ids (`__hoverN`) — the button's `cssState`
   *  reads `containsMouse` so `:hover` rules fire natively. Shared across one emit pass. */
  hoverCounter?: { n: number };
  /** Resource names declared in this component (`createResource`). `<Suspense>` gates its children on
   *  "no tracked resource is loading" — `!(<r>_loading || …)`. */
  resources?: string[];
  /** Mutable local variable names (from `analyzeMutableLocals`): `let x = <literal>` promoted to a
   *  `property var x` so handler closures share a persistent slot. Resolved bare like any cell. */
  mutableLocals?: Set<string>;
  /** Helper function names (from `analyzeHelpers`): top-level `function f(){}` and param/block arrows
   *  emitted as QML object methods. Used in `emitHandler` to recognise `onClick={add}` → `add()`. */
  helpers?: Set<string>;
  /** Where `onCleanup(fn)` registers: the enclosing effect's cleanup list property (inside an
   *  effect body), else the component's `__cleanups`. */
  cleanupList?: string;
  /** Solid API names imported by the module (local → exported). A call to one the emitter does not
   *  model fails loud instead of becoming a call to an undefined function. */
  solidApi?: Map<string, string>;
  /** `const c = children(() => props.children)` names: `{c()}` slots the children like `{props.children}`. */
  childrenAccessors?: Set<string>;
  /** The component references a component name as a VALUE somewhere (not as a tag): a runtime
   *  value may hold a component (<Dynamic> must not take it for a tag string). */
  componentsAsValues?: boolean;
  /** splitProps rest aliases → the keys split off (absent from the rest). */
  restExcludes?: Record<string, string[]>;
  /** Per instantiated component (use-name → the props it declares), for spread props `<C {...o}>`. */
  componentProps?: Map<string, Set<string>>;
  /** Collector: runtime helpers the emitted code uses (untrack → `__untrack`, batch → SolidBatch). */
  runtime?: { untrack: boolean; batch: boolean };
  /** Imported JS bindings mirrored as QML ES modules: local name → QML expression. */
  jsImports?: Record<string, string>;
  /** Mutable flag set by widget emitters when a T.* control is emitted.
   *  `flag`: any widget present → prepend Templates import.
   *  `calendar`: AbstractMonthGrid/AbstractDayOfWeekRow are used → needs Templates 6.3 (not 6.0). */
  usedWidgets?: { flag: boolean; calendar: boolean; popupWindow: boolean; extraImports?: Set<string>; widgetLib?: boolean };
  /** Button group names collected by emitRadioButton; emitComponentType reads this after
   *  the render pass to emit one T.ButtonGroup { id: __group_<name> } per unique name. */
  buttonGroups?: Map<string, string[]>; // group name -> radio ctlIds in DECLARATION order
}

/** A reactive cell / prop / mutable-local name as a QML reference: always BARE — it's a property on
 *  the component's QML root object, resolved by name through QML's object scope. One place owns this. */
// JS operator precedence (subset the transpiler emits) — larger binds tighter.
function binPrec(op: string): number {
  switch (op) {
    case "??": case "||": return 3;
    case "&&": return 4;
    case "==": case "!=": case "===": case "!==": return 8;
    case "<": case ">": case "<=": case ">=": case "in": case "instanceof": return 9;
    case "+": case "-": return 11;
    case "*": case "/": case "%": return 12;
    case "**": return 13;
    default: return 20;
  }
}

function cellRef(name: string, _scope: Scope): string {
  // A component's reactive cells / locals are properties on its QML root object; any closure in the
  // component's object tree (onCompleted, nested callbacks, Connections handlers) resolves them by
  // BARE name through QML's object scope chain. No `__self.` qualification — the object IS the class.
  return memberName(name);
}

/** A function/arrow's own identifier params shadow any same-named entry inherited from the
 *  enclosing scope — a module-const alias (`__const_NAME`), a mutable local, an event-handler
 *  substitution, … — exactly like real JS lexical scoping. `toName` must match whatever string the
 *  function's own signature uses for that param (callers emit the signature separately). One place
 *  owns this so every site that emits a function/arrow with real params applies it the same way. */
export function shadowOwnParams(scope: Scope, params: t.Node[], toName: (name: string) => string = safeName): Scope {
  let locals: Record<string, string> | undefined;
  for (const p of params) {
    // A destructured param (`({ a, b = 1 }) => …`) binds every name in its pattern.
    const names = t.isIdentifier(p) ? [p.name] : Object.keys(t.getBindingIdentifiers(p));
    for (const name of names) {
      locals = locals ?? { ...(scope.locals ?? {}) };
      locals[name] = t.isIdentifier(p) ? toName(name) : safeName(name);
    }
  }
  return locals ? { ...scope, locals } : scope;
}

/** A binding target — a declaration id, a function param, a for-of/catch head: an identifier or a
 *  destructuring pattern (`[a, b = 1, ...rest]`, `{ a, b: c = 2 }`), emitted as the same pattern
 *  (V4 destructures natively). Object REST (`{ a, ...rest }`) is not supported by V4 and fails. */
export function emitPattern(node: t.Node, scope: Scope): string {
  if (t.isIdentifier(node)) return safeName(node.name);
  if (t.isAssignmentPattern(node)) return `${emitPattern(node.left, scope)} = ${emitExpr(node.right, scope)}`;
  if (t.isRestElement(node)) return `...${emitPattern(node.argument, scope)}`;
  if (t.isArrayPattern(node)) return `[${node.elements.map((e) => (e ? emitPattern(e, scope) : "")).join(", ")}]`;
  if (t.isObjectPattern(node)) {
    const props = node.properties.map((p) => {
      if (t.isRestElement(p))
        throw new Error("object rest in a destructuring pattern (`{ a, ...rest }`) is not supported by the V4 engine — read the fields you need explicitly");
      const key = p.computed ? `[${emitExpr(p.key, scope)}]`
        : t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? JSON.stringify(p.key.value)
        : t.isNumericLiteral(p.key) ? String(p.key.value) : null;
      if (key === null) throw new Error(`unsupported destructuring key ${p.key.type}`);
      return `${key}: ${emitPattern(p.value, scope)}`;
    });
    return `{ ${props.join(", ")} }`;
  }
  if (t.isMemberExpression(node)) return emitExpr(node, scope); // `for (o.x of xs)`
  throw new Error(`unsupported binding pattern ${node.type}`);
}

/** Translate a Babel expression node to a QML-JS string, resolving names through the SymbolTable. */
// An operand whose natural precedence is looser than member access / call needs grouping when it
// becomes the OBJECT of `.`/`[]` or the CALLEE of `()`. (Identifiers, member/call chains, literals
// and already-grouped primaries don't.)
function parenObject(node: t.Node, emitted: string): string {
  const loose = t.isLogicalExpression(node) || t.isBinaryExpression(node) || t.isConditionalExpression(node)
    || t.isSequenceExpression(node) || t.isAssignmentExpression(node) || t.isArrowFunctionExpression(node)
    || t.isFunctionExpression(node) || t.isUnaryExpression(node) || t.isAwaitExpression(node)
    || t.isYieldExpression(node) || t.isObjectExpression(node);
  return loose ? `(${emitted})` : emitted;
}

export function emitExpr(node: t.Node, scope: Scope): string {
  if (t.isParenthesizedExpression(node)) return `(${emitExpr(node.expression, scope)})`;
  if (t.isNumericLiteral(node)) return String(node.value);
  if (t.isStringLiteral(node)) return JSON.stringify(node.value);
  if (t.isBooleanLiteral(node)) return String(node.value);
  if (t.isNullLiteral(node)) return "null";

  if (t.isArrayExpression(node)) {
    const elements = node.elements.map((e) => {
      if (!e) return "undefined";
      if (t.isSpreadElement(e)) return `...${emitExpr(e.argument, scope)}`;
      return emitExpr(e, scope);
    }).join(", ");
    return `[${elements}]`;
  }

  if (t.isObjectExpression(node)) {
    // V4 has no object spread: `{ ...a, x: 1, ...b }` is lowered to the equivalent
    // `Object.assign({}, a, { x: 1 }, b)` (same own-enumerable copy, same last-wins order).
    if (node.properties.some((p) => t.isSpreadElement(p))) {
      const parts: string[] = ["{}"];
      let run: (t.ObjectProperty | t.ObjectMethod)[] = [];
      const flush = () => { if (run.length) parts.push(emitExpr(t.objectExpression(run), scope)); run = []; };
      for (const p of node.properties) {
        if (t.isSpreadElement(p)) { flush(); parts.push(emitExpr(p.argument, scope)); }
        else run.push(p);
      }
      flush();
      return `Object.assign(${parts.join(", ")})`;
    }
    const props = node.properties.map((p) => {
      if (t.isSpreadElement(p)) throw new Error("unreachable");
      const key = p.computed ? `[${emitExpr(p.key, scope)}]`
        : t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? JSON.stringify(p.key.value)
        : t.isNumericLiteral(p.key) ? String(p.key.value) : null;
      if (key === null) throw new Error(`emitExpr: unsupported object key ${p.key.type}`);
      if (t.isObjectMethod(p)) {
        if (p.kind !== "method" || p.generator || p.async)
          throw new Error(`emitExpr: object ${p.kind === "method" ? (p.async ? "async method" : "generator method") : `${p.kind}ter`} not supported — use a plain method or a function property`);
        const params = p.params.map((q) => emitPattern(q, scope)).join(", ");
        return `${key}: function(${params}) { ${emitStatements(p.body.body, shadowOwnParams(scope, p.params))} }`;
      }
      return `${key}: ${t.isExpression(p.value) ? emitExpr(p.value, scope) : "undefined"}`; // shorthand `{x}` → x: x
    });
    return `({ ${props.join(", ")} })`;
  }
  if (t.isRegExpLiteral(node)) return `/${node.pattern}/${node.flags}`;
  if (t.isThisExpression(node)) return "this";
  if (t.isSequenceExpression(node)) return `(${node.expressions.map((e) => emitExpr(e, scope)).join(", ")})`;
  if (t.isNewExpression(node)) {
    const callee = t.isIdentifier(node.callee) || t.isMemberExpression(node.callee) ? emitExpr(node.callee, scope) : `(${emitExpr(node.callee, scope)})`;
    return `new ${callee}(${emitArgs(node.arguments, scope)})`;
  }
  // `a?.b`, `a?.[k]`, `f?.(x)` — V4 evaluates optional chains natively; only the names are resolved.
  if (t.isOptionalMemberExpression(node)) {
    const obj = parenObject(node.object, emitExpr(node.object, scope));
    const dot = node.optional ? "?." : ".";
    if (node.computed) return `${obj}${node.optional ? "?." : ""}[${emitExpr(node.property, scope)}]`;
    return `${obj}${dot}${(node.property as t.Identifier).name}`;
  }
  if (t.isOptionalCallExpression(node)) {
    const callee = parenObject(node.callee, emitExpr(node.callee, scope));
    return `${callee}${node.optional ? "?." : ""}(${emitArgs(node.arguments, scope)})`;
  }

  if (t.isIdentifier(node)) {
    if (scope.locals && node.name in scope.locals) return scope.locals[node.name];
    // An accessor name referenced WITHOUT a call (e.g. <For>'s index accessor passed bare:
    // `onClick={() => select(i)}`) — Solid semantics: this passes the ACCESSOR FUNCTION itself, so
    // the emitted value must still be callable later and read the CURRENT value then, not now.
    // Wrap the same fixed QML expression the `name()` call site resolves to (above) in a closure.
    if (scope.accessors && node.name in scope.accessors) return `(function() { return ${scope.accessors[node.name]} })`;
    const sym = scope.table.get(node.name);
    if (sym?.kind === "signal" || sym?.kind === "store") return cellRef(sym.name, scope); // reactive property read
    if (sym?.kind === "selector") // passed by value: still a function of the key
      return `(function(__k) { return ${emitExpr(t.callExpression(node, [t.identifier("__k")]), { ...scope, locals: { ...(scope.locals ?? {}), __k: "__k" } })} })`;
    if (scope.mutableLocals?.has(node.name)) return cellRef(node.name, scope); // mutable local → property read
    if (scope.jsImports && node.name in scope.jsImports) return scope.jsImports[node.name];
    return safeName(node.name);
  }

  // Expression-free template literal — a plain (usually multi-line) string.
  if (t.isTemplateLiteral(node) && node.expressions.length === 0) {
    return JSON.stringify(node.quasis.map((q) => q.value.cooked ?? q.value.raw).join(""));
  }

  if (t.isBinaryExpression(node) || t.isLogicalExpression(node)) {
    // Re-parenthesize by precedence: the AST has no parens, so `(slide + 2) % 3` would
    // otherwise flatten to `slide + 2 % 3` (a different expression).
    const prec = binPrec(node.operator);
    const guard = (child: t.Node, emitted: string, isRight: boolean): string => {
      if (t.isConditionalExpression(child) || t.isAssignmentExpression(child) || t.isSequenceExpression(child))
        return `(${emitted})`;
      if (t.isBinaryExpression(child) || t.isLogicalExpression(child)) {
        const p = binPrec(child.operator);
        if (p < prec) return `(${emitted})`;
        // Equal precedence on the RIGHT changes grouping (`a - (b - c)`, `"x" + (1 + 2)`)
        // except for the truly associative logical operators.
        if (p === prec && isRight && node.operator !== "&&" && node.operator !== "||")
          return `(${emitted})`;
      }
      return emitted;
    };
    const left = t.isPrivateName(node.left) ? node.left.id.name : guard(node.left, emitExpr(node.left, scope), false);
    return `${left} ${node.operator} ${guard(node.right, emitExpr(node.right, scope), true)}`;
  }

  if (t.isConditionalExpression(node)) {
    return `${emitExpr(node.test, scope)} ? ${emitExpr(node.consequent, scope)} : ${emitExpr(node.alternate, scope)}`;
  }

  if (t.isCallExpression(node) && t.isIdentifier(node.callee)) {
    const api = solidApiOf(node.callee.name, scope);
    // onCleanup(fn) → register a teardown on the current owner: the enclosing effect's own list
    // (run before its next run and on unmount), else the component's `__cleanups` (run on unmount).
    if (api === "onCleanup") {
      if (node.arguments.length !== 1) throw new Error("onCleanup takes exactly one function");
      return `${scope.cleanupList ?? "__cleanups"}.push(${emitExpr(node.arguments[0], scope)})`;
    }
    // untrack(fn) → its reads register no dependency (see component.ts `__untrack`).
    if (api === "untrack") {
      if (node.arguments.length !== 1) throw new Error("untrack takes exactly one function");
      if (scope.runtime) scope.runtime.untrack = true;
      return `__untrack(${accessorFn(node.arguments[0], scope)})`;
    }
    // batch(fn) → computations re-run once, after fn (runtime.ts BATCH_MODULE_SOURCE).
    if (api === "batch") {
      if (node.arguments.length !== 1) throw new Error("batch takes exactly one function");
      if (scope.runtime) scope.runtime.batch = true;
      return `SolidBatch.batch(${accessorFn(node.arguments[0], scope)})`;
    }
    if (api && !SOLID_EMITTED.has(api)) throw new Error(unsupportedSolidCall(node.callee.name, api));
    if (scope.accessors && node.callee.name in scope.accessors && node.arguments.length === 0)
      return scope.accessors[node.callee.name]; // <Index> item() → modelData
    const sym = scope.table.get(node.callee.name);
    if (sym?.kind === "signal" && node.arguments.length === 0) return cellRef(sym.name, scope); // count() -> count / __self.count
    if (sym?.kind === "memo" && node.arguments.length === 0) return cellRef(sym.name, scope);   // memo() -> memo property
    if (sym?.kind === "resource" && node.arguments.length === 0) return cellRef(sym.name, scope); // user() -> user (the resource value property)
    if (sym?.kind === "selector") {
      if (node.arguments.length !== 1) throw new Error(`${node.callee.name}(key): a selector takes one key`);
      const src = emitExpr(t.callExpression(sym.source, []), scope);
      const key = emitExpr(node.arguments[0], scope);
      return sym.fn ? `(${emitExpr(sym.fn, scope)})(${key}, ${src})` : `(${key} === ${src})`;
    }
    if (sym?.kind === "setter") return emitSetter(sym.signal, node.arguments[0], scope);
    if (sym?.kind === "storeSetter") return emitStoreSetter(sym.store, node.arguments, scope);
    if (sym?.kind === "derived") {                                                        // derived() -> inline body
      if (t.isBlockStatement(sym.body)) throw new Error("emitExpr: block-bodied derived accessor not supported yet");
      const inner: Scope = { ...scope, locals: { ...(scope.locals ?? {}) } };
      sym.params.forEach((p, i) => {
        const a = node.arguments[i];
        inner.locals![p] = a ? emitExpr(a, scope) : "undefined";
      });
      return `(${emitExpr(sym.body, inner)})`;
    }
  }

  // A zero-arg call on a context accessor member — `c.count()` (c ∈ ctxBindings, member is an
  // "accessor") → `__ctx_<ctx>.count`: drop the call, it's a reactive property read on the provider.
  if (t.isCallExpression(node) && t.isMemberExpression(node.callee) && !node.callee.computed
      && t.isIdentifier(node.callee.object) && t.isIdentifier(node.callee.property)
      && scope.ctxBindings && node.callee.object.name in scope.ctxBindings && node.arguments.length === 0) {
    const ctx = scope.ctxBindings[node.callee.object.name];
    const member = node.callee.property.name;
    if (scope.ctxValueShape?.[ctx]?.[member] === "accessor")
      return `__ctx_${safeName(ctx)}.${memberName(member)}`;
  }

  if (t.isCallExpression(node)) {
    const callee = parenObject(node.callee, emitExpr(node.callee, scope));
    return `${callee}(${emitArgs(node.arguments, scope)})`;
  }

  if (t.isMemberExpression(node)) {
    // A context member `c.member` (c ∈ ctxBindings) → `__ctx_<ctx>.member` (read through the
    // injected provider instance). Used as a value (e.g. `onClick={c.increment}` → a fn ref).
    if (!node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
        && scope.ctxBindings && node.object.name in scope.ctxBindings)
      return `__ctx_${safeName(scope.ctxBindings[node.object.name])}.${memberName(node.property.name)}`;
    if (!node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
        && (node.object.name === scope.propsParam || scope.propAliases?.has(node.object.name)))
      return cellRef(node.property.name, scope); // props.X / merged.X / local.X → property X
    // Null-safe resource read: `user().field` while loading (user undefined) must not crash — guard
    // the object with `(user || ({}))`. Bindings under <Show> still evaluate eagerly while loading.
    if (t.isCallExpression(node.object) && node.object.arguments.length === 0
        && t.isIdentifier(node.object.callee) && scope.table.get(node.object.callee.name)?.kind === "resource") {
      const ref = cellRef(node.object.callee.name, scope);
      if (node.computed) return `(${ref} || ({}))[${emitExpr(node.property, scope)}]`;
      return `(${ref} || ({})).${(node.property as t.Identifier).name}`;
    }
    // The object of a member access must be parenthesized when it binds LOOSER than `.`/`[]`
    // (a Logical/Conditional/Binary/Sequence/… object): `(a || b).x` must NOT flatten to
    // `a || b.x` (a precedence bug that silently changed the value — e.g. a scheme lookup
    // returning the object instead of its colour, painting the terminal black).
    const obj = parenObject(node.object, emitExpr(node.object, scope));
    if (node.computed) return `${obj}[${emitExpr(node.property, scope)}]`;
    return `${obj}.${(node.property as t.Identifier).name}`;
  }

  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) {
    if (node.generator) throw new Error("generator functions are not supported in component code — move them into a plain .ts module (loaded as a JS module, unmodified)");
    const params = node.params.map((p) => emitPattern(p, scope)).join(", ");
    const inner = shadowOwnParams(scope, node.params);
    if (t.isBlockStatement(node.body)) {
      // Block-bodied callback in an expression context (e.g. a .filter()/.map() argument). Reads
      // stay bare (the object scope resolves them).
      return `function(${params}) { ${emitStatements(node.body.body, inner)} }`;
    }
    return `function(${params}) { return ${emitExpr(node.body, inner)} }`;
  }

  // `!x` / `-x` / `typeof x` etc — prefix (or postfix) unary operators. Word operators
  // (typeof/void/delete) need the separating space — `typeof process` emitted as
  // `typeofprocess` (an undefined identifier) without it.
  if (t.isUnaryExpression(node)) {
    // The AST has no parens: a compound operand must be regrouped, or `!(a && b)` emits as
    // `!a && b` and `-(a + b)` as `-a + b` — a different value, silently.
    const a = node.argument;
    const compound = t.isBinaryExpression(a) || t.isLogicalExpression(a) || t.isConditionalExpression(a)
      || t.isAssignmentExpression(a) || t.isSequenceExpression(a) || t.isArrowFunctionExpression(a);
    const arg = compound ? `(${emitExpr(a, scope)})` : emitExpr(a, scope);
    const sep = /^[a-z]/.test(node.operator) ? " " : "";
    return node.prefix ? `${node.operator}${sep}${arg}` : `${arg}${sep}${node.operator}`;
  }

  // `x++` / `++x` / `x--` / `--x` — emit with the resolved argument (mutable local → __self.x)
  if (t.isUpdateExpression(node)) {
    const arg = emitExpr(node.argument, scope);
    return node.prefix ? `${node.operator}${arg}` : `${arg}${node.operator}`;
  }

  // `x = v` / `x += v` etc — emit with resolved left-hand side
  if (t.isAssignmentExpression(node)) {
    const left = emitExpr(node.left as t.Expression, scope);
    const right = emitExpr(node.right, scope);
    return `${left} ${node.operator} ${right}`;
  }

  // `` `a ${x} b` `` — V4 supports template literals, but the emit pipeline quotes strings and
  // resolves identifiers per-part, so lower to plain concatenation (web-identical semantics;
  // every interpolation coerces through string concat exactly like the runtime would).
  if (t.isTemplateLiteral(node)) {
    const parts: string[] = [];
    node.quasis.forEach((q, k) => {
      if (q.value.cooked) parts.push(JSON.stringify(q.value.cooked));
      const e = node.expressions[k];
      if (e && t.isExpression(e)) parts.push(`(${emitExpr(e, scope)})`);
    });
    if (parts.length === 0) return '""';
    // A lone interpolation still coerces to string ("" + x).
    if (parts.length === 1 && node.expressions.length === 1) return `("" + ${parts[0]})`;
    return parts.join(" + ");
  }

  // Fallback for the few node kinds not yet handled — keep it visible, don't silently drop.
  throw new Error(`unsupported expression in component code: ${node.type}${EXPR_HINT[node.type] ? ` — ${EXPR_HINT[node.type]}` : ""}`);
}

/** The Solid API a call's callee names, unless a local binding shadows the import. Without import
 *  information (a hand-built scope) only the three helpers the emitter lowers are recognised. */
function solidApiOf(name: string, scope: Scope): string | undefined {
  if (scope.table.has(name) || (scope.locals && name in scope.locals) || scope.helpers?.has(name)) return undefined;
  if (scope.solidApi) return scope.solidApi.get(name);
  return name === "onCleanup" || name === "untrack" || name === "batch" ? name : undefined;
}

/** Solid APIs that legitimately reach emitExpr as plain calls (handled by their own emitter). */
const SOLID_EMITTED = new Set(["onCleanup", "untrack", "batch"]);

function unsupportedSolidCall(local: string, api: string): string {
  const where = `\`${local}(…)\``;
  switch (api) {
    case "createRoot":
      return `${where}: createRoot is not supported natively — a component's computations live exactly as long as its QML object (there is no detached owner). Create the signals/effects in a component body.`;
    case "createMutable": case "modifyMutable":
      return `${where}: ${api} is not supported natively — QML bindings cannot observe in-place mutation of a plain object. Use createStore and its setter.`;
    case "getOwner": case "runWithOwner":
      return `${where}: ownership is not modelled natively (a component's computations are owned by its QML object).`;
    case "createSignal": case "createMemo": case "createEffect": case "createRenderEffect": case "createComputed":
    case "createSelector": case "createStore": case "createResource": case "onMount": case "mergeProps":
    case "splitProps": case "useContext": case "on":
      return `${where}: ${api} is only supported at the top level of a component body (\`const x = ${api}(…)\` / \`${api}(…);\`${api === "on" ? ", or as createEffect(on(…)) / createMemo(on(…))" : ""}) — here it would be a call to an undefined function.`;
    default:
      return `${where}: ${api} (from solid-js) is not supported natively.`;
  }
}

/** The function argument of untrack/batch: a function as written; a signal/memo/accessor name
 *  (`untrack(count)`) becomes a function that reads it. */
function accessorFn(arg: t.Node, scope: Scope): string {
  if (t.isIdentifier(arg)) {
    const k = scope.table.get(arg.name)?.kind;
    if (k === "signal" || k === "memo" || k === "resource" || k === "derived" || (scope.accessors && arg.name in scope.accessors))
      return `function() { return ${emitExpr(t.callExpression(arg, []), scope)} }`;
  }
  return emitExpr(arg, scope);
}

const EXPR_HINT: Record<string, string> = {
  BigIntLiteral: "the V4 engine has no BigInt",
  ClassExpression: "declare the class in a plain .ts module and import it (loaded as a JS module, unmodified)",
  TaggedTemplateExpression: "call the tag function explicitly",
  YieldExpression: "generators are not supported in component code",
};

/** Call/`new` arguments — a spread argument (`f(...xs)`) passes through (V4 spreads natively). */
function emitArgs(args: t.Node[], scope: Scope): string {
  return args.map((a) => (t.isSpreadElement(a) ? `...${emitExpr(a.argument, scope)}` : emitExpr(a, scope))).join(", ");
}

/** setStore(...) → reassign the store property to a NEW value so the `property var` notifies (QML
 *  can't track in-place mutation). Handles the forms used in practice: a (predicate, "key", updater)
 *  path setter; `produce(draft => mutate)`; a functional `(list) => newList`; and a plain replace. */
function emitStoreSetter(store: string, args: t.Node[], scope: Scope): string {
  const g = memberName(store);
  // path setter: set(pred, "key", updater) → map, immutably replacing the key on matching items.
  if (args.length >= 3 && t.isStringLiteral(args[1])) {
    const pred = args[0], upd = args[args.length - 1];
    const key = JSON.stringify(args[1].value);
    if ((t.isArrowFunctionExpression(pred) || t.isFunctionExpression(pred)) && (t.isArrowFunctionExpression(upd) || t.isFunctionExpression(upd)))
      return `${g} = ${g}.map(function(__t) { return (${emitExpr(pred, scope)})(__t) ? (function() { var __o = Object.assign({}, __t); __o[${key}] = (${emitExpr(upd, scope)})(__t[${key}]); return __o; })() : __t; })`;
  }
  if (args.length === 1) {
    const a = args[0];
    // produce(fn): mutate a shallow copy, then reassign.
    if (t.isCallExpression(a) && t.isIdentifier(a.callee, { name: "produce" }) && a.arguments[0])
      return `${g} = (function() { var __d = ${g}.slice(); (${emitExpr(a.arguments[0], scope)})(__d); return __d; })()`;
    // functional setter: (list) => newList.
    if (t.isArrowFunctionExpression(a) || t.isFunctionExpression(a))
      return `${g} = (${emitExpr(a, scope)})(${g})`;
    // replace with a value.
    if (t.isExpression(a)) return `${g} = ${emitExpr(a, scope)}`;
  }
  throw new Error(`store setter form not supported for ${store}`);
}

/** setX(value) / setX(prev => expr) → `<signal> = <value>` (the setter mutates its bare property). */
function emitSetter(signal: string, arg: t.Node | undefined, scope: Scope): string {
  // Qualify the write target with the component root id when set — a handler nested inside a
  // Template control (e.g. T.Dial's onMoved) resolves a bare `angle` to the CONTROL's read-only
  // `angle`, not the component's `angle` signal. Reads elsewhere stay bare (tracking).
  const target = scope.selfId ? `${scope.selfId}.${memberName(signal)}` : cellRef(signal, scope);
  if (arg && (t.isArrowFunctionExpression(arg) || t.isFunctionExpression(arg))) {
    const param = arg.params[0];
    const inner: Scope = { ...scope, locals: { ...(scope.locals ?? {}) } };
    if (param && t.isIdentifier(param)) inner.locals![param.name] = target; // prev → the property itself
    const body = t.isBlockStatement(arg.body)
      ? (() => { throw new Error("emitSetter: block-bodied updater not supported yet"); })()
      : emitExpr(arg.body, inner);
    return `${target} = ${body}`;
  }
  return `${target} = ${arg ? emitExpr(arg, scope) : "undefined"}`;
}
