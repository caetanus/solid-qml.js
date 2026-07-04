import * as t from "@babel/types";
import { safeName } from "../names/safe.ts";
import type { SymbolTable } from "../model/symbols.ts";

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
  /** Collector: ref variable names seen during emitQml (pushed by the container emitter when
   *  ref={x} is encountered). Read by emitComponentType after emitting the render tree to populate
   *  setupScope.locals so onMount bodies resolve the ref to its QML id. */
  refs?: string[];
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
  /** Imported JS bindings mirrored as QML ES modules: local name → QML expression. */
  jsImports?: Record<string, string>;
  /** Mutable flag set by widget emitters when a T.* control is emitted.
   *  `flag`: any widget present → prepend Templates import.
   *  `calendar`: AbstractMonthGrid/AbstractDayOfWeekRow are used → needs Templates 6.3 (not 6.0). */
  usedWidgets?: { flag: boolean; calendar: boolean; popupWindow: boolean };
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
  return safeName(name);
}

/** Translate a Babel expression node to a QML-JS string, resolving names through the SymbolTable. */
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
    const props = node.properties.map((p) => {
      if (!t.isObjectProperty(p) || p.computed) throw new Error("emitExpr: spread/computed object property not supported");
      const key = t.isIdentifier(p.key) ? p.key.name : t.isStringLiteral(p.key) ? JSON.stringify(p.key.value) : null;
      if (key === null) throw new Error("emitExpr: unsupported object key");
      return `${key}: ${t.isExpression(p.value) ? emitExpr(p.value, scope) : "undefined"}`; // shorthand `{x}` → x: x
    });
    return `({ ${props.join(", ")} })`;
  }

  if (t.isIdentifier(node)) {
    if (scope.locals && node.name in scope.locals) return scope.locals[node.name];
    const sym = scope.table.get(node.name);
    if (sym?.kind === "signal" || sym?.kind === "store") return cellRef(sym.name, scope); // reactive property read
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
    if (scope.accessors && node.callee.name in scope.accessors && node.arguments.length === 0)
      return scope.accessors[node.callee.name]; // <Index> item() → modelData
    const sym = scope.table.get(node.callee.name);
    if (sym?.kind === "signal" && node.arguments.length === 0) return cellRef(sym.name, scope); // count() -> count / __self.count
    if (sym?.kind === "memo" && node.arguments.length === 0) return cellRef(sym.name, scope);   // memo() -> memo property
    if (sym?.kind === "resource" && node.arguments.length === 0) return cellRef(sym.name, scope); // user() -> user (the resource value property)
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
      return `__ctx_${safeName(ctx)}.${safeName(member)}`;
  }

  if (t.isCallExpression(node)) {
    const callee = emitExpr(node.callee, scope);
    const args = node.arguments.map((a) => emitExpr(a, scope)).join(", ");
    return `${callee}(${args})`;
  }

  if (t.isMemberExpression(node)) {
    // A context member `c.member` (c ∈ ctxBindings) → `__ctx_<ctx>.member` (read through the
    // injected provider instance). Used as a value (e.g. `onClick={c.increment}` → a fn ref).
    if (!node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
        && scope.ctxBindings && node.object.name in scope.ctxBindings)
      return `__ctx_${safeName(scope.ctxBindings[node.object.name])}.${safeName(node.property.name)}`;
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
    const obj = emitExpr(node.object, scope);
    if (node.computed) return `${obj}[${emitExpr(node.property, scope)}]`;
    return `${obj}.${(node.property as t.Identifier).name}`;
  }

  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) {
    const params = node.params.map((p) => (t.isIdentifier(p) ? safeName(p.name) : "_")).join(", ");
    if (t.isBlockStatement(node.body)) {
      // Block-bodied callback in an expression context (e.g. a .filter()/.map() argument). Statements
      // are emitted inline here because expr.ts can't import stmt.ts (circular); only the subset
      // callbacks need is implemented. Reads stay bare (the object scope resolves them).
      const body = node.body.body.map((st) => emitInlineStmt(st, scope)).join(" ");
      return `function(${params}) { ${body} }`;
    }
    return `function(${params}) { return ${emitExpr(node.body, scope)} }`;
  }

  // `!x` / `-x` / `typeof x` etc — prefix (or postfix) unary operators
  if (t.isUnaryExpression(node)) {
    const arg = emitExpr(node.argument, scope);
    return node.prefix ? `${node.operator}${arg}` : `${arg}${node.operator}`;
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

  // Fallback for the few node kinds not yet handled — keep it visible, don't silently drop.
  throw new Error(`emitExpr: unsupported node ${node.type}`);
}

/** Minimal inline statement emitter for block-bodied callbacks inside expression contexts.
 *  Avoids a circular import with stmt.ts by only handling the subset of statements that appear in
 *  callback bodies: VariableDeclaration, ReturnStatement, ExpressionStatement. */
function emitInlineStmt(node: t.Statement, scope: Scope): string {
  if (t.isVariableDeclaration(node))
    return node.declarations.map((d) =>
      `var ${(d.id as t.Identifier).name} = ${d.init ? emitExpr(d.init, scope) : "undefined"};`).join(" ");
  if (t.isReturnStatement(node))
    return `return ${node.argument ? emitExpr(node.argument, scope) : "undefined"};`;
  if (t.isExpressionStatement(node))
    return `${emitExpr(node.expression, scope)};`;
  throw new Error(`emitInlineStmt: unsupported statement ${node.type}`);
}

/** setStore(...) → reassign the store property to a NEW value so the `property var` notifies (QML
 *  can't track in-place mutation). Handles the forms used in practice: a (predicate, "key", updater)
 *  path setter; `produce(draft => mutate)`; a functional `(list) => newList`; and a plain replace. */
function emitStoreSetter(store: string, args: t.Node[], scope: Scope): string {
  const g = safeName(store);
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
  const target = cellRef(signal, scope);
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
