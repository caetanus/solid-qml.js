// The C++ (AOT) back-end: a second emitter over the SAME normalized Babel AST that emit/qml.ts
// walks. Where the QML back-end prints a `.qml` string that V4 interprets at runtime, this prints
// C++ that constructs the identical widget tree directly — dropping the interpreter from the hot
// path (structure, bindings, handlers). Emission is a template pass, no new toolchain (per ROADMAP).
//
// Scope: M-AOT-0 — the counter construct set (component state, div/button/text elements, static
// props, string-concat bindings, setter handlers, nested component instances, a Window root). Any
// construct outside that set throws loudly (the M-AOT-1 gap queue), never emits silent wrong code.
//
// The generated shape is proven pixel-identical to the QML back-end by examples/aot/proof — this
// emitter reproduces that hand-written pattern from the AST.
import * as path from "node:path";
import { readFile as fsReadFile } from "node:fs/promises";
import * as t from "@babel/types";
import _generate from "@babel/generator";
import { normalize } from "../../babel/transform.ts";
import { analyzeProps, analyzeResources, analyzeSignals, collectFetcherDeps, type SymbolTable } from "../../model/symbols.ts";
import { hParts, isHCall } from "../../ast/h.ts";

const generate: typeof _generate = (_generate as unknown as { default?: typeof _generate }).default ?? _generate;

export interface GeneratedCpp {
  /** State classes (Q_OBJECT) — needs moc. */
  header: string;
  /** Build functions. */
  source: string;
  /** app_main.cpp — emitted only when an entry component has a <Window> root. */
  main: string | null;
  /** The entry component's build function + window metadata (for the driver / build wiring). */
  entry: { buildFn: string; width: number; height: number; title: string };
}

const TEXT_TAGS = new Set(["text", "span", "h1", "h2", "h3", "h4", "h5", "h6", "p", "cite", "bio"]);

interface CompInfo {
  name: string;
  fn: t.Function;
  render: t.CallExpression;
  /** The module AST the component lives in (to inline a createResource fetcher's decls). */
  file: t.File;
}

/** Every top-level component function (returns an h() call). */
function findComponents(ast: t.File): Map<string, CompInfo> {
  const out = new Map<string, CompInfo>();
  const consider = (fn: t.Function | null | undefined, name: string | null) => {
    if (!fn || !name || !t.isBlockStatement(fn.body)) return;
    for (const s of fn.body.body)
      if (t.isReturnStatement(s) && isHCall(s.argument)) {
        out.set(name, { name, fn, render: s.argument, file: ast });
        return;
      }
  };
  for (const node of ast.program.body) {
    if (t.isFunctionDeclaration(node) && node.id) consider(node, node.id.name);
    if (t.isExportNamedDeclaration(node) && node.declaration && t.isFunctionDeclaration(node.declaration) && node.declaration.id)
      consider(node.declaration, node.declaration.id.name);
    if (t.isExportDefaultDeclaration(node) && t.isFunctionDeclaration(node.declaration) && node.declaration.id)
      consider(node.declaration, node.declaration.id.name);
  }
  return out;
}

const cap = (s: string) => s.charAt(0).toUpperCase() + s.slice(1);
const cppStr = (s: string) => `QStringLiteral("${s.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n")}")`;

// Built-in identifier tags (control flow + Window) that are NOT followed as author components.
const BUILTIN_TAGS = new Set(["Show", "For", "Index", "Switch", "Match", "Suspense", "Dynamic", "Window", "Portal", "ErrorBoundary"]);

/** local binding name → imported specifier + original name (named/default imports). */
function collectImports(ast: t.File): Map<string, { spec: string; imported: string }> {
  const out = new Map<string, { spec: string; imported: string }>();
  for (const node of ast.program.body)
    if (t.isImportDeclaration(node))
      for (const s of node.specifiers) {
        if (t.isImportSpecifier(s)) out.set(s.local.name, { spec: node.source.value, imported: t.isIdentifier(s.imported) ? s.imported.name : s.imported.value });
        else if (t.isImportDefaultSpecifier(s)) out.set(s.local.name, { spec: node.source.value, imported: "default" });
      }
  return out;
}

/** Component-tag identifiers used anywhere in a render tree (excludes control-flow/built-in tags). */
function usedComponentTags(render: t.CallExpression, out: Set<string>): void {
  const walk = (n: t.Node): void => {
    if (isHCall(n)) { const { tag } = hParts(n); if (t.isIdentifier(tag) && !BUILTIN_TAGS.has(tag.name)) out.add(tag.name); }
    for (const key of Object.keys(n)) {
      const v = (n as unknown as Record<string, unknown>)[key];
      if (Array.isArray(v)) { for (const cc of v) if (cc && typeof (cc as t.Node).type === "string") walk(cc as t.Node); }
      else if (v && typeof (v as t.Node).type === "string") walk(v as t.Node);
    }
  };
  walk(render);
}

/** Resolve a relative specifier to an on-disk module (null for npm/builtin specifiers). */
async function resolveModule(dir: string, spec: string, readFile: (p: string) => Promise<string>): Promise<{ path: string; source: string } | null> {
  if (!spec.startsWith(".")) return null;
  const base = path.resolve(dir, spec);
  for (const cand of [`${base}.tsx`, `${base}.ts`, path.join(base, "index.tsx"), path.join(base, "index.ts")])
    try { return { path: cand, source: await readFile(cand) }; } catch { /* next */ }
  return null;
}

/** Follow relative imports of used component tags, collecting every author component across the graph
 *  into one name→component map (first definition wins on a name collision). */
async function loadGraph(entrySource: string, entryFile: string, readFile: (p: string) => Promise<string>): Promise<Map<string, CompInfo>> {
  const all = new Map<string, CompInfo>();
  const visited = new Set<string>();
  const load = async (absPath: string, src: string): Promise<void> => {
    if (visited.has(absPath)) return;
    visited.add(absPath);
    const { ast } = await normalize(src, absPath);
    if (!ast) return;
    const local = findComponents(ast);
    const imports = collectImports(ast);
    for (const [n, ci] of local) if (!all.has(n)) all.set(n, ci);
    const usedTags = new Set<string>();
    for (const ci of local.values()) usedComponentTags(ci.render, usedTags);
    for (const tagName of usedTags) {
      if (local.has(tagName)) continue;
      const imp = imports.get(tagName);
      if (!imp) continue;
      const resolved = await resolveModule(path.dirname(absPath), imp.spec, readFile);
      if (resolved) await load(resolved.path, resolved.source);
    }
  };
  await load(entryFile, entrySource);
  return all;
}

/** Expression scope: which identifiers are the component's reactive cells / props / setters, plus the
 *  owning state class name (connect targets like `&CounterState::countChanged`). */
interface Scope {
  table: SymbolTable;
  propsParam: string | null;
  props: Set<string>;
  components: Set<string>;
  stateClass: string;
  /** Local aliases (e.g. a functional-updater param → `state->events()`). */
  locals?: Record<string, string>;
  /** mergeProps/splitProps alias names whose `.member` reads resolve to props (`merged.greeting`). */
  propAliases?: Set<string>;
  /** Component names that own a state class (built as `buildX(ctx, new XState())` vs `buildX(ctx)`). */
  stateful: Set<string>;
  /** createResource names in this component (each owns state props r/r_loading/r_error). */
  resources?: string[];
}

/** Is `name` a state-backed reactive/prop cell (→ `state->name()`)? */
function isStateRead(name: string, sc: Scope): boolean {
  const sym = sc.table.get(name);
  return sc.props.has(name) || sc.resources?.includes(name) === true
    || (!!sym && (sym.kind === "signal" || sym.kind === "memo" || sym.kind === "store" || sym.kind === "resource"));
}

/** Translate a JS expression to a C++ QVariant expression (the sq:: runtime carries JS coercions). */
function emitExpr(node: t.Node, sc: Scope): string {
  if (t.isParenthesizedExpression(node)) return emitExpr(node.expression, sc);
  if (t.isStringLiteral(node)) return `QVariant(${cppStr(node.value)})`;
  if (t.isNumericLiteral(node)) return `QVariant(${Number.isInteger(node.value) ? node.value : `${node.value}`})`;
  if (t.isBooleanLiteral(node)) return `QVariant(${node.value})`;
  if (t.isIdentifier(node)) {
    if (sc.locals && node.name in sc.locals) return sc.locals[node.name];
    if (isStateRead(node.name, sc)) return `state->${node.name}()`;
    throw new Error(`AOT: unsupported identifier "${node.name}" (not a signal/prop of the component)`);
  }
  if (t.isConditionalExpression(node))
    return `(sq::truthy(${emitExpr(node.test, sc)}) ? ${emitExpr(node.consequent, sc)} : ${emitExpr(node.alternate, sc)})`;
  if (t.isUnaryExpression(node) && node.operator === "!") return `sq::not_(${emitExpr(node.argument, sc)})`;
  if (t.isUnaryExpression(node) && node.operator === "-" && t.isNumericLiteral(node.argument))
    return `QVariant(-${node.argument.value})`;
  if (t.isCallExpression(node) && t.isIdentifier(node.callee)) {
    const sym = sc.table.get(node.callee.name);
    // A local accessor call `item()` (a <For>/<Index> row param) → its bound value.
    if (node.arguments.length === 0 && sc.locals && node.callee.name in sc.locals) return sc.locals[node.callee.name];
    // `count()` — a signal/memo/store accessor call with no args → the state getter.
    if (node.arguments.length === 0 && isStateRead(node.callee.name, sc)) return `state->${node.callee.name}()`;
    // `doubleCount()` — a derived (a plain zero/param accessor) → inline its expression body, binding
    // any params to the call args (JS derived are just functions; we inline rather than emit a method).
    if (sym && sym.kind === "derived" && t.isExpression(sym.body)) {
      const locals = { ...(sc.locals ?? {}) };
      sym.params.forEach((pn, i) => { const a = node.arguments[i]; if (t.isExpression(a)) locals[pn] = emitExpr(a, sc); });
      return `(${emitExpr(sym.body, { ...sc, locals })})`;
    }
  }
  // `props.label` / `merged.greeting` (a mergeProps/splitProps alias) → the state getter.
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && (node.object.name === sc.propsParam || sc.propAliases?.has(node.object.name)))
    return `state->${node.property.name}()`;
  // `item.name` — member access on a <For> row local (an object item) → safe QVariantMap lookup.
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && sc.locals && node.object.name in sc.locals)
    return `sq::get(${sc.locals[node.object.name]}, "${node.property.name}")`;
  if (t.isArrayExpression(node)) {
    if (!node.elements.some((e) => t.isSpreadElement(e)))
      return `QVariant::fromValue(QVariantList{${node.elements.map((e) => e && t.isExpression(e) ? emitExpr(e, sc) : "QVariant()").join(", ")}})`;
    // A spread `[...items(), x]` builds the list imperatively (an IIFE keeps it an expression).
    const stmts = node.elements.map((e) => {
      if (t.isSpreadElement(e)) return `for (const auto &__e : ${emitExpr(e.argument, sc)}.toList()) __l.append(__e);`;
      return e && t.isExpression(e) ? `__l.append(${emitExpr(e, sc)});` : "";
    }).filter(Boolean).join(" ");
    return `[&]{ QVariantList __l; ${stmts} return QVariant::fromValue(__l); }()`;
  }
  // General `<expr>.member` (e.g. a resource accessor `user().avatar_url`) → safe QVariantMap lookup.
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.property) && t.isExpression(node.object))
    return `sq::get(${emitExpr(node.object, sc)}, "${node.property.name}")`;
  if (t.isLogicalExpression(node)) {
    const fn = { "&&": "and_", "||": "or_", "??": "nullish" }[node.operator];
    return `sq::${fn}(${emitExpr(node.left, sc)}, ${emitExpr(node.right, sc)})`;
  }
  if (t.isBinaryExpression(node) && t.isExpression(node.left)) {
    const op = ({ "+": "add", "-": "sub", "*": "mul", "/": "div", "%": "mod",
      "===": "strictEq", "!==": "strictNe", "==": "strictEq", "!=": "strictNe",
      "<": "lt", ">": "gt", "<=": "le", ">=": "ge" } as Record<string, string>)[node.operator];
    if (!op) throw new Error(`AOT: unsupported binary operator "${node.operator}"`);
    return `sq::${op}(${emitExpr(node.left, sc)}, ${emitExpr(node.right, sc)})`;
  }
  throw new Error(`AOT: unsupported expression node "${node.type}"`);
}

/** Collect the reactive dependency identifiers (signals/props) an expression reads. */
function collectDeps(node: t.Node, sc: Scope, out: Set<string>): void {
  if (t.isIdentifier(node) && isStateRead(node.name, sc)) out.add(node.name);
  if (t.isCallExpression(node) && t.isIdentifier(node.callee)) {
    if (node.arguments.length === 0 && isStateRead(node.callee.name, sc)) out.add(node.callee.name);
    // A derived accessor's deps are the deps of its (inlined) body.
    const sym = sc.table.get(node.callee.name);
    if (sym && sym.kind === "derived" && t.isExpression(sym.body)) collectDeps(sym.body, sc, out);
  }
  if (t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.object) && t.isIdentifier(node.property)
      && (node.object.name === sc.propsParam || sc.propAliases?.has(node.object.name)))
    out.add(node.property.name);
  for (const key of Object.keys(node)) {
    const v = (node as unknown as Record<string, unknown>)[key];
    if (Array.isArray(v)) for (const c of v) { if (c && typeof (c as t.Node).type === "string") collectDeps(c as t.Node, sc, out); }
    else if (v && typeof (v as t.Node).type === "string") collectDeps(v as t.Node, sc, out);
  }
}

interface ElemProps { classes: string[]; onClick: t.Node | undefined; }
function readProps(propsArg: t.Node | undefined): ElemProps {
  const out: ElemProps = { classes: [], onClick: undefined };
  if (!propsArg || !t.isObjectExpression(propsArg)) return out;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key)) continue;
    if (p.key.name === "class" && t.isStringLiteral(p.value)) out.classes = p.value.value.split(/\s+/).filter(Boolean);
    else if (p.key.name === "onClick") out.onClick = p.value;
  }
  return out;
}

const isElement = (n: t.Node) => isHCall(n);

/** Build a `setText(...)` string-concat binding + its dependency connects for the non-element
 *  children of a button/text. Returns the C++ statements (into `out`) targeting `btnVar`. */
function emitTextBinding(children: t.Node[], targetVar: string, sc: Scope, out: string[]): void {
  const parts = children.filter((c) => !isElement(c)).map((c) => {
    if (t.isStringLiteral(c)) return c.value.trim() || /\s/.test(c.value) ? `QVariant(${cppStr(c.value)})` : null;
    if (t.isJSXText(c)) { const s = c.value.replace(/\s+/g, " "); return s.trim() ? `QVariant(${cppStr(s)})` : null; }
    if (t.isExpression(c)) return emitExpr(c, sc);
    return null;
  }).filter((s): s is string => s !== null);
  // Fold as JS `"" + p0 + p1 + ...` so the result is always a string.
  let expr = "QVariant(QString())";
  for (const p of parts) expr = `sq::add(${expr}, ${p})`;
  const setText = `${targetVar}->setText(sq::str(${expr}));`;
  const deps = new Set<string>();
  for (const c of children) if (!isElement(c) && typeof (c as t.Node).type === "string") collectDeps(c, sc, deps);
  if (deps.size === 0) {
    out.push(setText);
    return;
  }
  // A connect-driven update lambda over each dependency's NOTIFY signal.
  out.push(`{ auto __upd = [${targetVar}, state] { ${setText} };`);
  for (const d of deps) out.push(`  QObject::connect(state, &${sc.stateClass}::${d}Changed, ${targetVar}, __upd);`);
  out.push(`  __upd(); }`);
}

/** Build context threaded through the element walker. `completes` records every item that still
 *  needs componentComplete(), in CREATION order; the build function completes them in REVERSE at the
 *  end (bottom-up), so each node's style resolution and layout see the fully-assembled tree — exactly
 *  the QML engine's build-then-complete lifecycle. (Inheritance relies on the parent link existing
 *  before a child completes.) */
interface Ctx { sc: Scope; ctx: string; out: string[]; fresh: () => string; completes: string[] }

const classLine = (v: string, classes: string[]) =>
  classes.length ? [`${v}->setCssClass(sq::classes({${classes.map((c) => `"${c}"`).join(", ")}}));`] : [];

/** Emit a container's children in order: element children as their own nodes, and each contiguous run
 *  of raw text/expression children as an anonymous `Css.CssText` node (cssPrimitive "" — inherits
 *  style, does not match type selectors), mirroring the QML back-end. */
function emitChildrenInto(parentVar: string, children: t.Node[], c: Ctx): void {
  let run: t.Node[] = [];
  const meaningful = (n: t.Node) =>
    (t.isStringLiteral(n) && (n.value.trim() || /\s/.test(n.value))) || (t.isJSXText(n) && n.value.trim())
    || (t.isExpression(n) && !t.isStringLiteral(n) && !t.isJSXText(n));
  const flush = () => {
    if (run.some(meaningful)) {
      const tv = c.fresh();
      c.out.push(`auto *${tv} = new QmlCss::CssText();`, `sq::begin(${tv}, ctx);`, `${tv}->setCssPrimitive(QStringLiteral(""));`);
      c.out.push(`sq::append(${parentVar}, ${tv});`);
      emitTextBinding(run, tv, c.sc, c.out);
      c.completes.push(tv);
    }
    run = [];
  };
  for (const n of children) {
    if (!isElement(n)) { run.push(n); continue; }
    flush();
    const { tag } = hParts(n as t.CallExpression);
    // <Show> is control flow, not an element: it appends each of its children to THIS container with a
    // `visible` guard (an invisible box leaves the layout — the engine already handles it).
    if (t.isIdentifier(tag, { name: "Show" })) { emitShowInto(parentVar, n as t.CallExpression, c); continue; }
    if (t.isIdentifier(tag, { name: "Suspense" })) { emitSuspenseInto(parentVar, n as t.CallExpression, c); continue; }
    // <For>/<Index> — a rebuild-on-change row factory appended to THIS container (Fase 1: destroy+
    // rebuild over the array; keyed reconcile is Fase 1.5).
    if (t.isIdentifier(tag, { name: "For" }) || t.isIdentifier(tag, { name: "Index" })) {
      emitRepeaterInto(parentVar, n as t.CallExpression, c);
      continue;
    }
    const cv = emitElement(n as t.CallExpression, c);
    c.out.push(`sq::append(${parentVar}, ${cv});`);
  }
  flush();
}

/** Read the `when` / `fallback` expression nodes from a <Show>'s props object. */
function showProps(propsArg: t.Node | undefined): { when: t.Expression | null; fallback: t.Expression | null } {
  let when: t.Expression | null = null, fallback: t.Expression | null = null;
  if (propsArg && t.isObjectExpression(propsArg))
    for (const p of propsArg.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) {
        if (p.key.name === "when") when = p.value;
        if (p.key.name === "fallback") fallback = p.value;
      }
  return { when, fallback };
}

/** Give a built child a reactive `visible` binding over the Show condition's dependencies. */
function emitVisibleGuard(childVar: string, whenNode: t.Expression, invert: boolean, c: Ctx): void {
  const cond = `sq::truthy(${emitExpr(whenNode, c.sc)})`;
  const val = invert ? `!${cond}` : cond;
  const deps = new Set<string>();
  collectDeps(whenNode, c.sc, deps);
  if (deps.size === 0) { c.out.push(`${childVar}->setVisible(${val});`); return; }
  c.out.push(`{ auto __vis = [${childVar}, state] { ${childVar}->setVisible(${val}); };`);
  for (const d of deps) c.out.push(`  QObject::connect(state, &${c.sc.stateClass}::${d}Changed, ${childVar}, __vis);`);
  c.out.push(`  __vis(); }`);
}

/** Expand a <Show> into `parentVar`: each truthy-branch child guarded on `when`, each fallback child on
 *  its inverse. Children may be elements or component instances (a fragment fallback is unwrapped). */
function emitShowInto(parentVar: string, showNode: t.CallExpression, c: Ctx): void {
  const { children } = hParts(showNode);
  const { when, fallback } = showProps(showNode.arguments[1]);
  if (!when) throw new Error("AOT: <Show> requires a `when` prop");
  for (const child of children)
    if (isElement(child)) { const cv = emitElement(child as t.CallExpression, c); c.out.push(`sq::append(${parentVar}, ${cv});`); emitVisibleGuard(cv, when, false, c); }
  if (fallback) {
    const fbNodes = t.isCallExpression(fallback) && isFragment(fallback) ? hParts(fallback).children : [fallback];
    for (const fb of fbNodes)
      if (isElement(fb)) { const cv = emitElement(fb as t.CallExpression, c); c.out.push(`sq::append(${parentVar}, ${cv});`); emitVisibleGuard(cv, when, true, c); }
  }
}

const isFragment = (n: t.Node): boolean => isElement(n) && (() => { const { tag } = hParts(n as t.CallExpression); return t.isIdentifier(tag, { name: "hFrag" }) || (t.isMemberExpression(tag) && t.isIdentifier(tag.property, { name: "Fragment" })); })();

/** Give a built child a reactive `visible` binding over a raw C++ condition string + its state deps. */
function emitRawGuard(childVar: string, condCpp: string, deps: string[], c: Ctx): void {
  if (deps.length === 0) { c.out.push(`${childVar}->setVisible(${condCpp});`); return; }
  c.out.push(`{ auto __vis = [${childVar}, state] { ${childVar}->setVisible(${condCpp}); };`);
  for (const d of deps) c.out.push(`  QObject::connect(state, &${c.sc.stateClass}::${d}Changed, ${childVar}, __vis);`);
  c.out.push(`  __vis(); }`);
}

/** <Suspense fallback={…}> — gate children on "no tracked resource is still loading", the fallback on
 *  the inverse. Reuses the Show mechanism; the condition is the component's resource loading flags. */
function emitSuspenseInto(parentVar: string, node: t.CallExpression, c: Ctx): void {
  const { children } = hParts(node);
  const loadingDeps = (c.sc.resources ?? []).map((r) => `${r}_loading`);
  const ready = loadingDeps.length ? loadingDeps.map((l) => `!sq::truthy(state->${l}())`).join(" && ") : "true";
  const loading = loadingDeps.length ? loadingDeps.map((l) => `sq::truthy(state->${l}())`).join(" || ") : "false";
  let fallback: t.Expression | null = null;
  const props = node.arguments[1];
  if (props && t.isObjectExpression(props))
    for (const p of props.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "fallback" }) && t.isExpression(p.value)) fallback = p.value;
  for (const child of children)
    if (isElement(child)) { const cv = emitElement(child as t.CallExpression, c); c.out.push(`sq::append(${parentVar}, ${cv});`); emitRawGuard(cv, `(${ready})`, loadingDeps, c); }
  if (fallback) {
    const fbNodes = isFragment(fallback) ? hParts(fallback as t.CallExpression).children : [fallback];
    for (const fb of fbNodes)
      if (isElement(fb)) { const cv = emitElement(fb as t.CallExpression, c); c.out.push(`sq::append(${parentVar}, ${cv});`); emitRawGuard(cv, `(${loading})`, loadingDeps, c); }
  }
}

/** Emit the QJSEngine sidecar for a component's createResource(s): the fetcher decls + a per-resource
 *  loader (`state.<r>_loading = true; Promise.resolve(fetcher(state.<src>)).then(v => state.<r> = v)…`)
 *  evaluated on the app engine with the state QObject bound, then run and re-run on source change. */
function emitResourceSidecar(resources: { name: string; source: string; fetcher: string }[], file: t.File, compName: string, out: string[]): void {
  // Inline each distinct fetcher's module-level decls once, in source order (deduped by node).
  const seen = new Set<t.Node>();
  const decls: string[] = [];
  for (const r of resources)
    for (const node of collectFetcherDeps(file, r.fetcher))
      if (!seen.has(node)) { seen.add(node); decls.push(generate(node, { concise: false }).code); }
  const loaders = resources.map((r) => (
    `function __load_${r.name}(){ state.${r.name}_loading = true; state.${r.name}_error = undefined; ` +
    `Promise.resolve(${r.fetcher}(state.${r.source})).then(function(v){ state.${r.name} = v; state.${r.name}_loading = false; })` +
    `.catch(function(e){ state.${r.name}_error = String(e); state.${r.name}_loading = false; }); }`
  )).join("\n  ");
  const returns = resources.map((r) => `${r.name}: __load_${r.name}`).join(", ");
  const factory = `(function(state){\n  ${decls.join("\n  ")}\n  ${loaders}\n  return { ${returns} };\n})`;

  out.push(
    `{ QJSEngine *__js = ctx->engine();`,
    `  QJSValue __loaders = __js->evaluate(QStringLiteral(R"SQJS(${factory})SQJS")).call(QJSValueList{ __js->newQObject(state) });`,
  );
  for (const r of resources) {
    out.push(
      `  QJSValue __load_${r.name} = __loaders.property(QStringLiteral("${r.name}"));`,
      `  QObject::connect(state, &${compName}State::${r.source}Changed, state, [__load_${r.name}]() mutable { __load_${r.name}.call(); });`,
      `  __load_${r.name}.call();`,
    );
  }
  out.push(`}`);
}

/** Expand a <For>/<Index> into `parentVar`: a runtime closure that (re)builds one row per model
 *  element on every change to the model's dependencies. For/Index share the emit — the difference is
 *  purely how the delegate reads the row (`item` vs `item()`), which is the same local binding here.
 *  Fase 1 is destroy-and-rebuild (fine at config-panel scale); keyed reconcile is Fase 1.5. */
function emitRepeaterInto(parentVar: string, node: t.CallExpression, c: Ctx): void {
  const { children } = hParts(node);
  let eachNode: t.Expression | null = null;
  const props = node.arguments[1];
  if (props && t.isObjectExpression(props))
    for (const p of props.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key, { name: "each" }) && t.isExpression(p.value)) eachNode = p.value;
  const delegate = children.find((ch) => t.isArrowFunctionExpression(ch) || t.isFunctionExpression(ch)) as
    t.ArrowFunctionExpression | t.FunctionExpression | undefined;
  if (!eachNode || !delegate) throw new Error("AOT: <For>/<Index> needs an `each` prop and a delegate function");
  const body = delegate.body;
  if (!isHCall(body)) throw new Error("AOT: <For>/<Index> delegate must return a single element");

  const id = c.fresh();
  const rows = `__rows_${id}`, rebuild = `__rebuild_${id}`;
  const [p0, p1] = delegate.params;
  const locals = { ...(c.sc.locals ?? {}) };
  if (p0 && t.isIdentifier(p0)) locals[p0.name] = "__item";      // `item` / `item()` → the row value
  if (p1 && t.isIdentifier(p1)) locals[p1.name] = "QVariant(__i)"; // `index` / `i` → the row index

  // Build the row subtree (synchronously — it lives inside the runtime rebuild loop, not the initial
  // deferred pass): assemble, append to the container, then complete bottom-up.
  const sub: Ctx = { sc: { ...c.sc, locals }, ctx: c.ctx, out: [], fresh: c.fresh, completes: [] };
  const rowVar = emitElement(body, sub);
  sub.out.push(`sq::append(${parentVar}, ${rowVar});`);
  for (const v of [...sub.completes].reverse()) sub.out.push(`sq::complete(${v});`);
  sub.out.push(`${rows}->append(${rowVar});`);

  c.out.push(
    `auto *${rows} = new QList<QQuickItem *>();`,
    `auto ${rebuild} = [=] {`,
    `    for (auto *__r : *${rows}) { __r->setParentItem(nullptr); __r->deleteLater(); }`,
    `    ${rows}->clear();`,
    `    const QVariantList __model = ${emitExpr(eachNode, c.sc)}.toList();`,
    `    for (int __i = 0; __i < __model.size(); ++__i) {`,
    `        const QVariant __item = __model.at(__i);`,
    ...sub.out.map((l) => `        ${l}`),
    `    }`,
    `};`,
  );
  const deps = new Set<string>();
  collectDeps(eachNode, c.sc, deps);
  for (const d of deps) c.out.push(`QObject::connect(state, &${c.sc.stateClass}::${d}Changed, state, ${rebuild});`);
  c.out.push(`${rebuild}();`);
}

/** Emit the C++ that builds one element subtree into a fresh var; returns that var name. Completion is
 *  deferred (recorded in c.completes) so the whole subtree is assembled before anything completes. */
function emitElement(node: t.CallExpression, c: Ctx): string {
  const { sc, out } = c;
  const { tag, props: propsArg, children } = hParts(node);

  // Nested component instance: `<Counter label="A" />`. buildX returns an already-complete subtree.
  if (t.isIdentifier(tag) && sc.components.has(tag.name)) {
    const v = c.fresh();
    // A stateless component (no signals/props) has no state class → `buildX(ctx)`.
    if (!sc.stateful.has(tag.name)) {
      out.push(`auto *${v} = build${tag.name}(ctx);`);
      return v;
    }
    const stateVar = `${v}_st`;
    out.push(`auto *${stateVar} = new ${tag.name}State();`);
    if (propsArg && t.isObjectExpression(propsArg)) {
      for (const p of propsArg.properties)
        if (t.isObjectProperty(p) && t.isIdentifier(p.key) && p.key.name !== "children" && t.isExpression(p.value))
          out.push(`${stateVar}->set${cap(p.key.name)}(${emitExpr(p.value, sc)});`);
    }
    out.push(`auto *${v} = build${tag.name}(ctx, ${stateVar});`, `${stateVar}->setParent(${v}); // lifetime tied to the built item`);
    return v;
  }
  if (!t.isStringLiteral(tag)) throw new Error(`AOT: unsupported tag ${tag.type}`);
  const tagName = tag.value;
  const p = readProps(propsArg);
  const v = c.fresh();

  if (tagName === "div") {
    out.push(`auto *${v} = new SolidWidgets::Div();`, `sq::begin(${v}, ctx);`, ...classLine(v, p.classes));
    emitChildrenInto(v, children, c);
    c.completes.push(v);
    return v;
  }
  if (tagName === "button") {
    out.push(`auto *${v} = new SolidWidgets::Button();`, `sq::begin(${v}, ctx);`, ...classLine(v, p.classes));
    if (p.onClick) {
      if (!t.isArrowFunctionExpression(p.onClick) && !t.isFunctionExpression(p.onClick)) throw new Error("AOT: onClick must be an inline arrow");
      if (t.isBlockStatement(p.onClick.body)) throw new Error("AOT: block-bodied onClick not supported");
      out.push(`QObject::connect(${v}, &SolidWidgets::Button::clicked, state, [state] { ${emitHandler(p.onClick.body, sc)} });`);
    }
    // Match the QML declaration order: the `text` property first, then the element children.
    emitTextBinding(children, v, sc, out);
    for (const n of children) if (isElement(n)) { const cv = emitElement(n as t.CallExpression, c); out.push(`sq::append(${v}, ${cv});`); }
    c.completes.push(v);
    return v;
  }
  if (tagName === "input") { emitTextControl(propsArg, p, v, c, false); c.completes.push(v); return v; }
  if (tagName === "textarea") { emitTextControl(propsArg, p, v, c, true); c.completes.push(v); return v; }
  if (tagName === "img") { emitImage(propsArg, p, v, c); c.completes.push(v); return v; }
  if (TEXT_TAGS.has(tagName)) {
    out.push(`auto *${v} = new SolidWidgets::Text();`, `sq::begin(${v}, ctx);`, ...classLine(v, p.classes));
    if (tagName !== "text") out.push(`${v}->setCssPrimitive(${cppStr(tagName)});`);
    emitTextBinding(children, v, sc, out);
    c.completes.push(v);
    return v;
  }
  throw new Error(`AOT: unsupported element <${tagName}>`);
}

/** A setter-call handler body (`setCount(count() + 1)`) → `state->setCount(...);`. */
function emitHandler(body: t.Expression, sc: Scope): string {
  if (t.isCallExpression(body) && t.isIdentifier(body.callee)) {
    const sym = sc.table.get(body.callee.name);
    if (sym && sym.kind === "setter") {
      const arg = body.arguments[0];
      if (!arg || !t.isExpression(arg)) throw new Error("AOT: setter handler needs one expression arg");
      // Functional-updater form `setX(prev => expr)` → inline with prev bound to the current getter.
      if ((t.isArrowFunctionExpression(arg) || t.isFunctionExpression(arg)) && !t.isBlockStatement(arg.body)) {
        const param = arg.params[0] && t.isIdentifier(arg.params[0]) ? arg.params[0].name : null;
        const inner: Scope = param ? { ...sc, locals: { ...(sc.locals ?? {}), [param]: `state->${sym.signal}()` } } : sc;
        return `state->set${cap(sym.signal)}(${emitExpr(arg.body, inner)});`;
      }
      return `state->set${cap(sym.signal)}(${emitExpr(arg, sc)});`;
    }
  }
  throw new Error(`AOT: unsupported handler body "${body.type}" (only a signal setter call)`);
}

/** Is `node` an `<eventParam>.target.value` / `.currentTarget.value` read (the DOM idiom the input
 *  handler uses to get the field's current text)? */
function isEventValue(node: t.Node, eventParam: string | null): boolean {
  return !!eventParam && t.isMemberExpression(node) && !node.computed && t.isIdentifier(node.property, { name: "value" })
    && t.isMemberExpression(node.object) && !node.object.computed && t.isIdentifier(node.object.object, { name: eventParam })
    && t.isIdentifier(node.object.property) && (node.object.property.name === "target" || node.object.property.name === "currentTarget");
}

/** A controlled `<input>`'s edit handler `(e) => setX(e.target.value)` → `state->setX(field->text())`
 *  (the event's value read resolves to the field's live text). */
function emitInputHandler(fn: t.ArrowFunctionExpression | t.FunctionExpression, fieldVar: string, sc: Scope): string {
  if (t.isBlockStatement(fn.body)) throw new Error("AOT: block-bodied input handler not supported");
  const ev = fn.params[0] && t.isIdentifier(fn.params[0]) ? fn.params[0].name : null;
  const body = fn.body;
  if (t.isCallExpression(body) && t.isIdentifier(body.callee)) {
    const sym = sc.table.get(body.callee.name);
    if (sym && sym.kind === "setter") {
      const arg = body.arguments[0];
      if (arg && isEventValue(arg, ev)) return `state->set${cap(sym.signal)}(QVariant(${fieldVar}->text()));`;
      if (arg && t.isExpression(arg)) return `state->set${cap(sym.signal)}(${emitExpr(arg, sc)});`;
    }
  }
  throw new Error("AOT: unsupported <input> handler (only setX(e.target.value))");
}

/** Controlled `<input>` → SolidWidgets::TextField: a value binding pushes the signal into the field,
 *  and textEdited runs the onInput handler back into the signal. Special input types (checkbox/radio/
 *  range/number/date → dedicated widgets) fail loud for now. */
/** Controlled `<input>` (TextField, textEdited) or `<textarea>` (TextArea, textChanged). */
function emitTextControl(propsArg: t.Node | undefined, p: ElemProps, v: string, c: Ctx, isTextArea: boolean): void {
  let value: t.Expression | null = null, onInput: t.Node | undefined, placeholder: string | null = null;
  let type = "text", disabled = false, readOnly = false;
  if (propsArg && t.isObjectExpression(propsArg))
    for (const pr of propsArg.properties) {
      if (!t.isObjectProperty(pr) || !t.isIdentifier(pr.key)) continue;
      const k = pr.key.name;
      if (k === "value" && t.isExpression(pr.value)) value = pr.value;
      else if (k === "onInput" || k === "onChange") onInput = pr.value;
      else if (k === "placeholder" && t.isStringLiteral(pr.value)) placeholder = pr.value.value;
      else if (k === "type" && t.isStringLiteral(pr.value)) type = pr.value.value;
      else if (k === "disabled") disabled = true;
      else if (k === "readOnly" || k === "readonly") readOnly = true;
    }
  if (!isTextArea && ["checkbox", "radio", "range", "number", "date"].includes(type))
    throw new Error(`AOT: <input type="${type}"> not supported yet (needs the dedicated widget)`);

  const cls = isTextArea ? "TextArea" : "TextField";
  // TextArea has no textEdited (it extends TextEdit) — onInput/onChange map to textChanged. The
  // controlled re-assert (setText) is change-gated (`if (m_text == v) return`), so it never loops.
  const editSignal = isTextArea ? "textChanged" : "textEdited";
  c.out.push(`auto *${v} = new SolidWidgets::${cls}();`, `sq::begin(${v}, ctx);`, ...classLine(v, p.classes));
  if (!isTextArea && type === "password") c.out.push(`${v}->setEchoMode(2);`); // TextInput.Password
  if (placeholder) c.out.push(`${v}->setPlaceholder(${cppStr(placeholder)});`);
  if (readOnly) c.out.push(`${v}->setReadOnly(true);`);
  if (disabled) c.out.push(`${v}->setEnabled(false);`);
  if (value) emitReactiveSet(`${v}->setText(sq::str(EXPR))`, value, v, c);
  if (onInput) {
    if (!t.isArrowFunctionExpression(onInput) && !t.isFunctionExpression(onInput)) throw new Error("AOT: onInput must be an inline arrow");
    c.out.push(`QObject::connect(${v}, &SolidWidgets::${cls}::${editSignal}, state, [${v}, state] { ${emitInputHandler(onInput, v, c.sc)} });`);
  }
}

/** `<img src={u} />` → SolidWidgets::Image (QUrl src); the src is a (possibly reactive) string. */
function emitImage(propsArg: t.Node | undefined, p: ElemProps, v: string, c: Ctx): void {
  let src: t.Expression | null = null;
  if (propsArg && t.isObjectExpression(propsArg))
    for (const pr of propsArg.properties)
      if (t.isObjectProperty(pr) && t.isIdentifier(pr.key, { name: "src" }) && t.isExpression(pr.value)) src = pr.value;
  c.out.push(`auto *${v} = new SolidWidgets::Image();`, `sq::begin(${v}, ctx);`, ...classLine(v, p.classes));
  if (src) emitReactiveSet(`${v}->setSrc(QUrl(sq::str(EXPR)))`, src, v, c);
}

/** Emit a property set that reacts to a value expression's deps. `setterTpl` has `EXPR` where the
 *  emitted value goes. With no deps it's a one-shot set (no `state` capture — works in stateless
 *  components); with deps it's a connect-driven lambda re-run on each dependency's NOTIFY. */
function emitReactiveSet(setterTpl: string, node: t.Expression, targetVar: string, c: Ctx): void {
  const setter = setterTpl.replace("EXPR", emitExpr(node, c.sc));
  const deps = new Set<string>();
  collectDeps(node, c.sc, deps);
  if (deps.size === 0) { c.out.push(`${setter};`); return; }
  c.out.push(`{ auto __set = [${targetVar}, state] { ${setter}; };`);
  for (const d of deps) c.out.push(`  QObject::connect(state, &${c.sc.stateClass}::${d}Changed, ${targetVar}, __set);`);
  c.out.push(`  __set(); }`);
}

interface WindowMeta { width: number; height: number; title: string; child: t.CallExpression }
/** Read a <Window> root's props and its single element child. */
function readWindow(render: t.CallExpression): WindowMeta {
  const { props, children } = hParts(render);
  let width = 640, height = 480, title = "";
  if (props && t.isObjectExpression(props))
    for (const p of props.properties)
      if (t.isObjectProperty(p) && t.isIdentifier(p.key) && t.isExpression(p.value)) {
        if (p.key.name === "width" && t.isNumericLiteral(p.value)) width = p.value.value;
        if (p.key.name === "height" && t.isNumericLiteral(p.value)) height = p.value.value;
        if (p.key.name === "title" && t.isStringLiteral(p.value)) title = p.value.value;
      }
  const child = children.find(isElement);
  if (!child) throw new Error("AOT: <Window> needs one element child");
  return { width, height, title, child: child as t.CallExpression };
}

export interface GenerateCppOptions {
  /** Read a module's source by absolute path (override in tests). Defaults to fs. */
  readFile?: (absPath: string) => Promise<string>;
}

export async function generateCpp(source: string, filename: string, opts: GenerateCppOptions = {}): Promise<GeneratedCpp> {
  const readFile = opts.readFile ?? ((p) => fsReadFile(p, "utf8"));
  // Follow relative imports so an entry that composes components from other files (the real gallery
  // shape) emits every author component across the graph — not just the entry file's.
  const comps = await loadGraph(source, filename, readFile);
  if (comps.size === 0) throw new Error("AOT: no components found");
  const componentNames = new Set(comps.keys());

  // Entry = a component whose render root is <Window> (identifier "Window").
  let entryName: string | null = null;
  for (const [name, info] of comps) {
    const { tag } = hParts(info.render);
    if (t.isIdentifier(tag, { name: "Window" })) { entryName = name; break; }
  }
  if (!entryName) throw new Error("AOT: no <Window>-rooted entry component (M-AOT-0 needs an app window)");

  // Pre-pass: which components own a state class (have signals/stores or read props → QVariant state).
  const stateful = new Set<string>();
  for (const [name, info] of comps) {
    const table = analyzeSignals(info.fn);
    const propsInfo = analyzeProps(info.fn);
    const hasCells = [...table.values()].some((s) => s.kind === "signal" || s.kind === "store")
      || propsInfo.used.some((n) => n !== "children") || analyzeResources(info.fn).length > 0;
    if (hasCells) stateful.add(name);
  }

  const headers: string[] = [];
  const sources: string[] = [];
  const forwards: string[] = [];

  for (const [name, info] of comps) {
    const table = analyzeSignals(info.fn);
    const propsInfo = analyzeProps(info.fn);
    const propNames = propsInfo.used.filter((n) => n !== "children");
    // The reactive cells that become QVariant state properties (signals/memos/stores), plus props.
    // A prop with a mergeProps default seeds the state member with that default (props override it).
    const cells = new Map<string, t.Expression | null>();
    for (const sym of table.values())
      if (sym.kind === "signal" || sym.kind === "store") cells.set(sym.name, sym.init ?? null);
    for (const n of propNames) if (!cells.has(n)) cells.set(n, propsInfo.defaults[n] ?? null);
    // Each createResource owns three state props: the value, a loading flag (starts true so the
    // Suspense fallback shows first), and an error slot. The async fetcher runs on the QJSEngine
    // sidecar and writes these back through the QObject (the C++ bindings react via NOTIFY).
    const resources = analyzeResources(info.fn);
    for (const r of resources) {
      cells.set(r.name, null);
      cells.set(`${r.name}_loading`, t.booleanLiteral(true));
      cells.set(`${r.name}_error`, null);
    }

    const sc: Scope & { table: SymbolTable; propsParam: string | null; props: Set<string>; components: Set<string> } = {
      stateClass: `${name}State`,
      table,
      propsParam: propsInfo.param,
      props: new Set(propNames),
      components: componentNames,
      stateful,
      ...(resources.length ? { resources: resources.map((r) => r.name) } : {}),
      ...(propsInfo.aliases.length ? { propAliases: new Set(propsInfo.aliases) } : {}),
    };

    const isEntry = name === entryName;
    const hasState = cells.size > 0;

    // ── State class ────────────────────────────────────────────────────────────────────────────
    if (hasState) {
      const props: string[] = [];
      const getset: string[] = [];
      const signals: string[] = [];
      const members: string[] = [];
      for (const [cell, init] of cells) {
        props.push(`    Q_PROPERTY(QVariant ${cell} READ ${cell} WRITE set${cap(cell)} NOTIFY ${cell}Changed)`);
        getset.push(
          `    QVariant ${cell}() const { return m_${cell}; }`,
          `    void set${cap(cell)}(const QVariant &v) { if (m_${cell} == v) return; m_${cell} = v; emit ${cell}Changed(); }`,
        );
        signals.push(`    void ${cell}Changed();`);
        const initExpr = init ? emitInitLiteral(init) : "";
        members.push(`    QVariant m_${cell}${initExpr ? ` = ${initExpr}` : ""};`);
      }
      headers.push(
        `class ${name}State : public QObject\n{\n    Q_OBJECT`,
        props.join("\n"),
        `public:\n    explicit ${name}State(QObject *parent = nullptr) : QObject(parent) {}`,
        getset.join("\n"),
        `signals:\n${signals.join("\n")}`,
        `private:\n${members.join("\n")}\n};\n`,
      );
    }

    // ── Build function ─────────────────────────────────────────────────────────────────────────
    const out: string[] = [];
    let n = 0;
    const fresh = () => `v${n++}`;
    const completes: string[] = [];
    const c: Ctx = { sc, ctx: "ctx", out, fresh, completes };
    let signature: string, retVar: string;
    if (isEntry) {
      // <Window> root → build the window-primitive box and the child subtree inside it.
      const win = readWindow(info.render);
      out.push(`auto *winBox = new QmlCss::CssRect();`, `sq::begin(winBox, ctx);`,
        `winBox->setCssPrimitive(QStringLiteral("window"));`, `winBox->setCssClass(sq::classes({"qml-window"}));`);
      completes.push("winBox"); // created first → completes last (bottom-up)
      const childVar = emitElement(win.child, c);
      out.push(`sq::append(winBox, ${childVar});`);
      retVar = "winBox";
      signature = `QQuickItem *build${name}(QQmlContext *ctx)`;
    } else {
      retVar = emitElement(info.render, c);
      signature = hasState
        ? `QQuickItem *build${name}(QQmlContext *ctx, ${name}State *state)`
        : `QQuickItem *build${name}(QQmlContext *ctx)`;
    }
    // Complete the fully-assembled tree bottom-up (reverse creation order).
    for (const v of [...completes].reverse()) out.push(`sq::complete(${v});`);
    // createResource: the async fetcher is genuinely dynamic JS (fetch/await/closures) — the "dynamic
    // frontier". It runs UNCHANGED on the QJSEngine sidecar (V4 stays in the binary for QtQml anyway),
    // with the state QObject exposed so the blob's writes flow back through NOTIFY into the C++ bindings.
    if (resources.length) emitResourceSidecar(resources, info.file, name, out);
    out.push(`return ${retVar};`);
    forwards.push(`${signature};`);
    sources.push(`${signature}\n{\n    ${out.join("\n    ")}\n}\n`);
  }

  const entryInfo = comps.get(entryName)!;
  const win = readWindow(entryInfo.render);

  const header = [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    "#pragma once",
    "",
    "#include <QObject>",
    "#include <QVariant>",
    "#include <QtQml/qqml.h>",
    "",
    "class QQmlContext;",
    "class QQuickItem;",
    "",
    "namespace aot {",
    "",
    ...headers,
    `QQuickItem *build${entryName}(QQmlContext *ctx);`,
    "",
    "} // namespace aot",
    "",
  ].join("\n");

  const src = [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    '#include "generated.h"',
    "",
    '#include "aot/sqbuild.h"',
    '#include "aot/sqruntime.h"',
    "",
    '#include "qmlcss/cssrect.h"',
    '#include "qmlcss/csstext.h"',
    '#include "widgets/button.h"',
    '#include "widgets/primitives.h"',
    '#include "widgets/textinputs.h"',
    "",
    "#include <QJSEngine>",
    "#include <QJSValue>",
    "#include <QList>",
    "#include <QQmlContext>",
    "#include <QQmlEngine>",
    "#include <QUrl>",
    "",
    "namespace aot {",
    "",
    "// Forward declarations (build order independent of component declaration order).",
    ...forwards,
    "",
    ...sources,
    "} // namespace aot",
    "",
  ].join("\n");

  const anyResources = [...comps.values()].some((ci) => analyzeResources(ci.fn).length > 0);
  const main = emitMain(entryName, win, anyResources);
  return { header, source: src, main, entry: { buildFn: `build${entryName}`, width: win.width, height: win.height, title: win.title } };
}

/** A pure-literal initializer for a state member (`property var count: 0`). */
function emitInitLiteral(node: t.Expression): string {
  if (t.isNumericLiteral(node)) return `${Number.isInteger(node.value) ? node.value : node.value}`;
  if (t.isStringLiteral(node)) return cppStr(node.value);
  if (t.isBooleanLiteral(node)) return `${node.value}`;
  if (t.isUnaryExpression(node) && node.operator === "-" && t.isNumericLiteral(node.argument)) return `-${node.argument.value}`;
  // Array-literal signal init (`createSignal(["a", "b"])`) → a QVariantList.
  if (t.isArrayExpression(node) && node.elements.every((e) => e && t.isExpression(e)))
    return `QVariant::fromValue(QVariantList{${node.elements.map((e) => emitInitLiteral(e as t.Expression)).join(", ")}})`;
  // Object-literal init (`createSignal([{ name: "Ada" }])`) → a QVariantMap.
  if (t.isObjectExpression(node) && node.properties.every((p) => t.isObjectProperty(p) && !p.computed && (t.isIdentifier(p.key) || t.isStringLiteral(p.key)) && t.isExpression(p.value)))
    return `QVariant::fromValue(QVariantMap{${node.properties.map((p) => {
      const op = p as t.ObjectProperty;
      const key = t.isIdentifier(op.key) ? op.key.name : (op.key as t.StringLiteral).value;
      return `{${cppStr(key)}, ${emitInitLiteral(op.value as t.Expression)}}`;
    }).join(", ")}})`;
  return ""; // undefined default
}

function emitMain(entryName: string, win: WindowMeta, installShims: boolean): string {
  return [
    "// Generated by the AOT (C++) back-end. Do not edit by hand.",
    "// app_main: mirrors src/loader.cpp minus QML loading — wires engine/theme/layout, builds the",
    "// generated scene in C++, hosts it in a QQuickWindow, supports the offscreen --grab/--click.",
    '#include "generated.h"',
    "",
    '#include "aot/sqbuild.h"',
    '#include "qmlcss/QMLCss.h"',
    '#include "qmlcss/csslayout.h"',
    '#include "qmlcss/csstheme.h"',
    '#include "shims/tabstop.h"',
    ...(installShims ? [
      '#include "shims/jspolyfill.h"',
      '#include "shims/webfetch.h"',
      '#include "shims/webplatform.h"',
      '#include "shims/webtimers.h"',
    ] : []),
    '#include "widgets/focusring.h"',
    '#include "widgets/solidwidgets.h"',
    "",
    "#include <QApplication>",
    "#include <QCommandLineParser>",
    "#include <QMouseEvent>",
    "#include <QQmlContext>",
    "#include <QQmlEngine>",
    "#include <QQuickItem>",
    "#include <QQuickWindow>",
    "#include <QTimer>",
    "",
    "int main(int argc, char **argv)",
    "{",
    "    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);",
    "    QApplication app(argc, argv);",
    "    QmlCss::registerTypes();",
    "    SolidWidgets::registerTypes();",
    "",
    "    QCommandLineParser parser;",
    "    parser.addHelpOption();",
    '    parser.addOption({ QStringLiteral("css"), QStringLiteral("CSS file(s), layered."), QStringLiteral("path") });',
    `    parser.addOption({ QStringLiteral("width"), QStringLiteral("Window width."), QStringLiteral("px"), QStringLiteral("${win.width}") });`,
    `    parser.addOption({ QStringLiteral("height"), QStringLiteral("Window height."), QStringLiteral("px"), QStringLiteral("${win.height}") });`,
    '    parser.addOption({ QStringLiteral("grab"), QStringLiteral("Render one frame to PNG and exit."), QStringLiteral("png") });',
    '    parser.addOption({ QStringLiteral("click"), QStringLiteral("Synthesize a left click at \\"x,y\\" (repeatable)."), QStringLiteral("x,y") });',
    "    parser.process(app);",
    "",
    '    const int w = parser.value(QStringLiteral("width")).toInt();',
    '    const int h = parser.value(QStringLiteral("height")).toInt();',
    "",
    "    QQmlEngine engine;",
    "    QmlCss::CssTheme theme;",
    "    QmlCss::CssLayoutEngine layout(&theme);",
    "    SolidTabstop solidTabstop;",
    "    QQmlContext *ctx = engine.rootContext();",
    '    ctx->setContextProperty(QStringLiteral("cssTheme"), &theme);',
    '    ctx->setContextProperty(QStringLiteral("cssLayout"), &layout);',
    '    ctx->setContextProperty(QStringLiteral("solidTabstop"), &solidTabstop);',
    ...(installShims ? [
      "    // Browser shims for the createResource sidecar's residual JS (fetch/timers/TextDecoder).",
      "    engine.installExtensions(QJSEngine::ConsoleExtension);",
      "    JsPolyfill::install(&engine);",
      "    WebPlatform::install(&engine);",
      "    WebTimers::install(&engine);",
      "    WebFetch::install(&engine);",
    ] : []),
    '    theme.loadLayered(parser.values(QStringLiteral("css")));',
    "",
    "    QQuickWindow window;",
    "    window.setWidth(w);",
    "    window.setHeight(h);",
    `    window.setTitle(${cppStr(win.title)});`,
    "    theme.setViewport(w, h);",
    "    QObject::connect(&window, &QQuickWindow::widthChanged, &theme, [&] { theme.setViewport(window.width(), window.height()); });",
    "    QObject::connect(&window, &QQuickWindow::heightChanged, &theme, [&] { theme.setViewport(window.width(), window.height()); });",
    "",
    `    QQuickItem *root = aot::build${entryName}(ctx);`,
    "    root->setParentItem(window.contentItem());",
    "    root->setSize(QSizeF(w, h));",
    "    QObject::connect(&window, &QQuickWindow::widthChanged, root, [root, &window] { root->setWidth(window.width()); });",
    "    QObject::connect(&window, &QQuickWindow::heightChanged, root, [root, &window] { root->setHeight(window.height()); });",
    "",
    "    // Desktop tab-focus chrome (mirrors the generated Window's Tabstop + focus-on-load): the",
    "    // Tabstop overlay tracks the focused control and paints the ::tab-stop ring.",
    "    auto *tabstop = new SolidWidgets::Tabstop();",
    "    sq::begin(tabstop, ctx);",
    "    tabstop->setWindow(&window);",
    "    tabstop->setParentItem(window.contentItem());",
    "    sq::complete(tabstop);",
    "    if (solidTabstop.enabled())",
    "        QTimer::singleShot(0, &window, [&window] {",
    "            if (auto *f = window.contentItem()->nextItemInFocusChain(true)) f->forceActiveFocus(Qt::TabFocusReason);",
    "        });",
    "    window.show();",
    "",
    '    const int grabMs = qEnvironmentVariableIntValue("SQ_GRAB_MS") > 0 ? qEnvironmentVariableIntValue("SQ_GRAB_MS") : 1400;',
    '    const QStringList clicks = parser.values(QStringLiteral("click"));',
    "    if (!clicks.isEmpty()) {",
    "        QTimer::singleShot(grabMs / 2, &window, [&window, clicks] {",
    "            for (const QString &c : clicks) {",
    '                const QStringList xy = c.split(QLatin1Char(\',\'));',
    "                if (xy.size() != 2) continue;",
    "                const QPointF p(xy[0].toDouble(), xy[1].toDouble());",
    "                const QPointF g = window.mapToGlobal(p);",
    "                QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);",
    "                QMouseEvent release(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);",
    "                QCoreApplication::sendEvent(&window, &press);",
    "                QCoreApplication::sendEvent(&window, &release);",
    "            }",
    "        });",
    "    }",
    '    if (parser.isSet(QStringLiteral("grab"))) {',
    '        const QString grabPath = parser.value(QStringLiteral("grab"));',
    "        QTimer::singleShot(grabMs, &window, [&window, grabPath] {",
    "            const QImage frame = window.grabWindow();",
    "            if (frame.save(grabPath)) qInfo(\"aot: wrote %s\", qUtf8Printable(grabPath));",
    "            else qWarning(\"aot: failed to write %s\", qUtf8Printable(grabPath));",
    "            QCoreApplication::quit();",
    "        });",
    "    }",
    "    return app.exec();",
    "}",
    "",
  ].join("\n");
}
