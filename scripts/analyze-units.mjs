// Dry-run reusability visitor. Walks each example and reports the "reusable units" it finds — every
// function that returns JSX — classifying each (app root / component / <For> delegate / render-prop),
// with its props, whether it holds state, whether it slots children, and how many times it's used.
// This validates the classification BEFORE we change codegen from inlining to one-QML-type-per-unit.
//
// Usage: node scripts/analyze-units.mjs [file.tsx ...]   (defaults to examples/*.tsx + src/main.tsx)
import ts from "typescript";
import { readFileSync, readdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function defaultFiles() {
  const ex = readdirSync(path.join(root, "examples"))
    .filter((f) => f.endsWith(".tsx") && f !== "gallery.tsx")
    .map((f) => `examples/${f}`);
  return [...ex.sort(), "src/main.tsx"];
}

const files = process.argv.slice(2).length ? process.argv.slice(2) : defaultFiles();

const isJsx = (n) => ts.isJsxElement(n) || ts.isJsxSelfClosingElement(n) || ts.isJsxFragment(n);
const unwrap = (n) => { while (n && ts.isParenthesizedExpression(n)) n = n.expression; return n; };

function analyze(file) {
  const src = ts.createSourceFile(file, readFileSync(path.join(root, file), "utf8"),
    ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);

  // The JSX a function returns (expression-bodied arrow, or a block's `return <JSX>`), else null.
  function returnedJsx(fn) {
    if (!fn.body) return null;
    if (!ts.isBlock(fn.body)) { const b = unwrap(fn.body); return isJsx(b) ? b : null; }
    for (const s of fn.body.statements)
      if (ts.isReturnStatement(s) && s.expression) { const e = unwrap(s.expression); if (isJsx(e)) return e; }
    return null;
  }
  function rootTag(jsx) {
    if (ts.isJsxFragment(jsx)) return "<>";
    const op = ts.isJsxElement(jsx) ? jsx.openingElement : jsx;
    return op.tagName.getText(src);
  }
  // Does a subtree call one of the given functions (createSignal, …)?
  function callsAny(node, names) {
    let hit = false;
    (function w(n) {
      if (hit) return;
      if (ts.isCallExpression(n) && ts.isIdentifier(n.expression) && names.includes(n.expression.text)) hit = true;
      else ts.forEachChild(n, w);
    })(node);
    return hit;
  }
  // props.X accesses + whether props.children is used, given the props param name.
  function propUse(fn, paramName) {
    const props = new Set();
    let children = false;
    if (!paramName) return { props, children };
    (function w(n) {
      if (ts.isPropertyAccessExpression(n) && ts.isIdentifier(n.expression) && n.expression.text === paramName) {
        if (n.name.text === "children") children = true; else props.add(n.name.text);
      }
      ts.forEachChild(n, w);
    })(fn.body ?? fn);
    return { props, children };
  }

  // Name + kind of a JSX-returning function from its surroundings.
  function describe(fn, jsx) {
    const tag = rootTag(jsx);
    // FunctionDeclaration name, or `const X = (…) => <…>`.
    let name = ts.isFunctionDeclaration(fn) && fn.name ? fn.name.text
      : (fn.parent && ts.isVariableDeclaration(fn.parent) && ts.isIdentifier(fn.parent.name) ? fn.parent.name.text : null);

    if (name && tag === "Window") return { name, kind: "app" };
    if (name) return { name, kind: "component" };

    // Anonymous: a <For>/<Index> delegate (child render fn) or a render-prop (attribute), or bootstrap.
    let p = fn.parent;
    if (p && ts.isJsxExpression(p) && p.parent && (ts.isJsxElement(p.parent) || ts.isJsxFragment(p.parent))) {
      const host = ts.isJsxElement(p.parent) ? p.parent.openingElement.tagName.getText(src) : "<>";
      return { name: `${host} delegate`, kind: /^(For|Index)$/.test(host) ? "delegate" : "render-prop(child)" };
    }
    if (p && ts.isJsxAttribute(p) && p.name) return { name: `${p.name.getText(src)}=`, kind: "render-prop(attr)" };
    if (p && ts.isJsxExpression(p) && p.parent && ts.isJsxAttribute(p.parent))
      return { name: `${p.parent.name.getText(src)}=`, kind: "render-prop(attr)" };
    if (p && ts.isCallExpression(p) && ts.isIdentifier(p.expression) && p.expression.text === "render")
      return { name: "render()", kind: "bootstrap" };
    return { name: "(anonymous)", kind: "component" };
  }

  // Collect every JSX-returning function.
  const units = [];
  (function walk(n) {
    if (ts.isFunctionDeclaration(n) || ts.isArrowFunction(n) || ts.isFunctionExpression(n)) {
      const jsx = returnedJsx(n);
      if (jsx) {
        const { name, kind } = describe(n, jsx);
        const param = n.parameters[0] && ts.isIdentifier(n.parameters[0].name) ? n.parameters[0].name.text : null;
        const { props, children } = propUse(n, param);
        units.push({
          name, kind, root: rootTag(jsx),
          props: [...props],
          children,
          state: callsAny(n, ["createSignal", "createStore", "createResource"]),
          memo: callsAny(n, ["createMemo"]),
          fn: n,
        });
      }
    }
    ts.forEachChild(n, walk);
  })(src);

  // How many times each named component is instantiated (a JSX tag matching its name).
  const useCount = new Map();
  (function walk(n) {
    if (ts.isJsxElement(n) || ts.isJsxSelfClosingElement(n)) {
      const op = ts.isJsxElement(n) ? n.openingElement : n;
      const tag = op.tagName.getText(src);
      useCount.set(tag, (useCount.get(tag) ?? 0) + 1);
    }
    ts.forEachChild(n, walk);
  })(src);

  return { units, useCount };
}

const KIND_W = 18;
for (const file of files) {
  const { units, useCount } = analyze(file);
  console.log(`\n═══ ${file} ═══`);
  for (const u of units) {
    if (u.kind === "bootstrap") continue;
    const uses = u.kind === "delegate" ? "per-item"
      : u.kind.startsWith("render-prop") ? "1 (conditional)"
      : `${useCount.get(u.name) ?? 0}×`;
    const flags = [
      u.state ? "state" : "",
      u.memo ? "memo" : "",
      u.children ? "children" : "",
      u.props.length ? `props:{${u.props.join(",")}}` : "",
    ].filter(Boolean).join("  ");
    console.log(
      `  ${("[" + u.kind + "]").padEnd(KIND_W)} ${u.name.padEnd(20)} → <${u.root}>`.padEnd(60)
      + `  uses:${String(uses).padEnd(16)} ${flags}`,
    );
  }
}
console.log("");
