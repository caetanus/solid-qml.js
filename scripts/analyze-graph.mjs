// Dry-run module resolver: from a QML entry, follow JS imports across files and build the component
// dependency graph (who renders whom), resolving `{ App as Hello }` aliases. This validates the
// resolution the multi-file transpiler will use to emit one QML per component (named after it) with
// imports mirroring the graph. Usage: node scripts/analyze-graph.mjs [entry.tsx]
import ts from "typescript";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const entry = path.resolve(root, process.argv[2] ?? "src/mainqml.tsx");

const cache = new Map();
const parse = (file) => {
  if (!cache.has(file))
    cache.set(file, ts.createSourceFile(file, fs.readFileSync(file, "utf8"), ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX));
  return cache.get(file);
};
const isJsx = (n) => ts.isJsxElement(n) || ts.isJsxSelfClosingElement(n) || ts.isJsxFragment(n);
const unwrap = (n) => { while (n && ts.isParenthesizedExpression(n)) n = n.expression; return n; };

function returnedJsx(fn) {
  if (!fn.body) return null;
  if (!ts.isBlock(fn.body)) { const b = unwrap(fn.body); return isJsx(b) ? b : null; }
  for (const s of fn.body.statements)
    if (ts.isReturnStatement(s) && s.expression) { const e = unwrap(s.expression); if (isJsx(e)) return e; }
  return null;
}

function resolveImport(fromFile, spec) {
  if (!spec.startsWith(".")) return null; // node_modules (solid-js, etc.) are not transpiled
  const base = path.resolve(path.dirname(fromFile), spec);
  for (const ext of ["", ".tsx", ".ts", ".jsx", ".js"]) {
    try { if (fs.statSync(base + ext).isFile()) return base + ext; } catch { /* keep trying */ }
  }
  return null;
}

// localName -> { file, exportName }
function importsOf(sf, file) {
  const m = new Map();
  for (const s of sf.statements) {
    if (!ts.isImportDeclaration(s) || !ts.isStringLiteral(s.moduleSpecifier) || !s.importClause) continue;
    const f = resolveImport(file, s.moduleSpecifier.text);
    if (!f) continue;
    const c = s.importClause;
    if (c.name) m.set(c.name.text, { file: f, exportName: "default" });
    if (c.namedBindings && ts.isNamedImports(c.namedBindings))
      for (const el of c.namedBindings.elements)
        m.set(el.name.text, { file: f, exportName: (el.propertyName ?? el.name).text });
  }
  return m;
}

// component functions defined in a file: name -> fn node
function localsOf(sf) {
  const m = new Map();
  for (const s of sf.statements) {
    if (ts.isFunctionDeclaration(s) && s.name && returnedJsx(s)) m.set(s.name.text, s);
    if (ts.isVariableStatement(s))
      for (const d of s.declarationList.declarations)
        if (ts.isIdentifier(d.name) && d.initializer
            && (ts.isArrowFunction(d.initializer) || ts.isFunctionExpression(d.initializer)) && returnedJsx(d.initializer))
          m.set(d.name.text, d.initializer);
  }
  return m;
}

function exportedComponent(sf, exportName) {
  const locals = localsOf(sf);
  if (exportName !== "default") return locals.get(exportName) ?? null;
  for (const s of sf.statements) {
    if (ts.isExportAssignment(s) && ts.isIdentifier(s.expression)) return locals.get(s.expression.text) ?? null;
    if (ts.isFunctionDeclaration(s) && returnedJsx(s)
        && ts.getModifiers(s)?.some((mod) => mod.kind === ts.SyntaxKind.DefaultKeyword)) return s;
  }
  return null;
}

// Capitalized JSX tags used in a component's body (its component deps), excluding <Window>.
function usedComponents(fn) {
  const jsx = returnedJsx(fn);
  const used = [];
  (function w(n) {
    if (!n) return;
    if (ts.isJsxElement(n) || ts.isJsxSelfClosingElement(n)) {
      const tag = (ts.isJsxElement(n) ? n.openingElement : n).tagName.getText();
      if (/^[A-Z]/.test(tag) && tag !== "Window" && !used.includes(tag)) used.push(tag);
    }
    ts.forEachChild(n, w);
  })(jsx);
  return used;
}

// Walk the graph from the entry's main component.
const graph = [];
const seen = new Set();
function visit(qmlName, fn, file) {
  const key = `${file}#${qmlName}`;
  if (seen.has(key)) return;
  seen.add(key);
  const sf = parse(file);
  const locals = localsOf(sf);
  const imports = importsOf(sf, file);
  const deps = [];
  for (const tag of usedComponents(fn)) {
    let depFn = null;
    let depFile = file;
    if (locals.has(tag)) depFn = locals.get(tag);
    else if (imports.has(tag)) {
      const imp = imports.get(tag);
      depFile = imp.file;
      depFn = exportedComponent(parse(imp.file), imp.exportName);
    }
    deps.push(tag);
    if (depFn) visit(tag, depFn, depFile);
    else console.warn(`  ! unresolved <${tag}> in ${path.relative(root, file)}`);
  }
  graph.push({ qmlName, file: path.relative(root, file), root: (function () {
    const j = returnedJsx(fn); if (!j || ts.isJsxFragment(j)) return "<>";
    return (ts.isJsxElement(j) ? j.openingElement : j).tagName.getText();
  })(), deps });
}

// Entry: the function that returns <Window> (or the last JSX-returning fn).
const entrySf = parse(entry);
const entryLocals = localsOf(entrySf);
let mainFn = null, mainName = "App";
for (const [name, fn] of entryLocals) {
  const j = returnedJsx(fn);
  if (j && !ts.isJsxFragment(j) && (ts.isJsxElement(j) ? j.openingElement : j).tagName.getText() === "Window") {
    mainFn = fn; mainName = name;
  }
}
if (!mainFn) { for (const [name, fn] of entryLocals) { mainFn = fn; mainName = name; } }

visit(mainName, mainFn, entry);

console.log(`\n═══ component graph from ${path.relative(root, entry)} ═══`);
for (const c of graph)
  console.log(`  ${c.qmlName}.qml`.padEnd(20) + `<${c.root}>`.padEnd(12) + `(${c.file})` + (c.deps.length ? `  → ${c.deps.join(", ")}` : ""));
console.log("");
