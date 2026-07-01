import { transformFromAstSync } from "@babel/core";
import _generate from "@babel/generator";
import _traverse from "@babel/traverse";
import presetEnv from "@babel/preset-env";
import * as t from "@babel/types";

const generate: typeof _generate = (_generate as any).default ?? _generate;
const traverse: typeof _traverse = (_traverse as any).default ?? _traverse;

const APPLY_HELPER = "__v4ApplySpread";

export function emitV4Module(file: t.File): string {
  const lowered = lowerSyntaxWithBabel(file);
  const polyfills = collectPolyfills(lowered);
  cleanupForQmlV4(lowered);
  const needsApplyNormalizer = normalizeTypedArrayApply(lowered);
  rewriteGlobalXhr(lowered);
  rewriteRegExpConstructor(lowered);
  exposeDefaultAsNamed(lowered);
  const code = generate(lowered, { compact: false, comments: false }).code;
  return `${applyNormalizerPrelude(needsApplyNormalizer)}${polyfillPrelude(polyfills)}${code}`;
}

// V4's Function.prototype.apply reads the right argument COUNT from a TypedArray but every spread
// element comes through as 0 (plain arrays and array-likes are fine) — and apply is frozen, so it
// cannot be patched. This silently breaks the common bytes->string idiom
// `String.fromCharCode.apply(null, uint8array)` that npm packages (e.g. js-base64) rely on. Rewrite
// each `x.apply(thisArg, args)` call so the argument list is normalized to a real array at runtime;
// the helper is a no-op for anything that isn't a TypedArray, so it is safe to apply blanket.
function normalizeTypedArrayApply(file: t.File): boolean {
  let used = false;
  traverse(file, {
    CallExpression(path) {
      const node = path.node;
      if (!t.isMemberExpression(node.callee) || node.callee.computed) return;
      if (!t.isIdentifier(node.callee.property, { name: "apply" })) return;
      if (node.arguments.length !== 2) return;
      const argsArg = node.arguments[1];
      if (!t.isExpression(argsArg)) return;
      // Already wrapped (e.g. re-visited after replacement) — leave it.
      if (t.isCallExpression(argsArg) && t.isIdentifier(argsArg.callee, { name: APPLY_HELPER })) return;
      path.get("arguments.1").replaceWith(t.callExpression(t.identifier(APPLY_HELPER), [t.cloneNode(argsArg)]));
      used = true;
    },
  });
  return used;
}

// QML provides a weak global `XMLHttpRequest`, and that global is frozen (non-configurable) so it
// can't be replaced — not from JS, not from the C++ host. The loader instead installs a fetch-backed
// drop-in under `__SqXMLHttpRequest`; rewrite free references to `XMLHttpRequest` in mirrored npm
// code to it so packages (axios/superagent/…) ride our networking. Locally bound names (a package
// that imports/declares its own `XMLHttpRequest`) are left alone, and `typeof XMLHttpRequest` feature
// checks still work because `__SqXMLHttpRequest` is a real global function.
function rewriteGlobalXhr(file: t.File): void {
  traverse(file, {
    ReferencedIdentifier(path) {
      if (path.node.name !== "XMLHttpRequest") return;
      if (path.scope.getBinding("XMLHttpRequest")) return;
      path.node.name = "__SqXMLHttpRequest";
    },
  });
}

// V4's JS RegExp captures named groups positionally but never populates `match.groups`, and the
// builtin `RegExp.prototype.exec` is frozen so it can't be wrapped. Packages that build their regex
// dynamically (`new RegExp("...(?<name>...)")`) therefore read `undefined.name` and throw — Babel
// can't lower a runtime-string pattern either. Route construction through the loader's `__SqRegExp`,
// which builds the same RegExp and (only when it has named groups) synthesises `.groups` from a
// PCRE2-derived name->index map. Locally-bound `RegExp` is left alone.
function rewriteRegExpConstructor(file: t.File): void {
  const isFreeRegExp = (path: { node: { callee: t.Node }; scope: { getBinding(n: string): unknown } }) =>
    t.isIdentifier(path.node.callee, { name: "RegExp" }) && !path.scope.getBinding("RegExp");
  traverse(file, {
    NewExpression(path) {
      if (isFreeRegExp(path)) path.node.callee = t.identifier("__SqRegExp");
    },
    CallExpression(path) {
      if (isFreeRegExp(path)) path.node.callee = t.identifier("__SqRegExp");
    },
  });
}

function applyNormalizerPrelude(needed: boolean): string {
  if (!needed) return "";
  return [
    "// V4 fix: Function.prototype.apply does not spread a TypedArray's values (right length, but",
    "// every element reads as 0), and apply is frozen so it can't be patched. Normalize the argument",
    "// list to a real array; a no-op for anything that isn't a TypedArray view.",
    `function ${APPLY_HELPER}(a) {`,
    "  return (a != null && typeof a === \"object\" && ArrayBuffer.isView(a) && !(a instanceof DataView))",
    "    ? Array.prototype.slice.call(a) : a;",
    "}",
    "",
  ].join("\n");
}

function lowerSyntaxWithBabel(file: t.File): t.File {
  const out = transformFromAstSync(file, undefined, {
    ast: true,
    code: false,
    babelrc: false,
    configFile: false,
    sourceType: "module",
    presets: [[presetEnv, {
      bugfixes: true,
      modules: false,
      targets: { ie: "11" },
      useBuiltIns: false,
    }]],
  });
  if (!out?.ast) throw new Error("node mirror compat: Babel preset-env did not return an AST");
  return out.ast as t.File;
}

function cleanupForQmlV4(file: t.File): void {
  traverse(file, {
    RegExpLiteral(path) {
      if (path.node.flags.includes("v")) path.node.flags = path.node.flags.replace("v", "u");
    },
    CallExpression(path) {
      const node = path.node;
      if (t.isMemberExpression(node.callee) && !node.callee.computed && t.isIdentifier(node.callee.property, { name: "at" })
          && node.arguments.length === 1 && t.isExpression(node.arguments[0])) {
        const obj = t.cloneNode(node.callee.object as t.Expression);
        const idx = atIndex(t.cloneNode(node.arguments[0] as t.Expression), t.cloneNode(node.callee.object as t.Expression));
        path.replaceWith(t.memberExpression(obj, idx, true));
        return;
      }
      if (t.isMemberExpression(node.callee) && !node.callee.computed && t.isIdentifier(node.callee.property, { name: "replaceAll" })
          && node.arguments.length === 2 && t.isExpression(node.arguments[0]) && t.isExpression(node.arguments[1])) {
        const split = t.callExpression(
          t.memberExpression(t.cloneNode(node.callee.object as t.Expression), t.identifier("split")),
          [t.cloneNode(node.arguments[0])],
        );
        path.replaceWith(t.callExpression(t.memberExpression(split, t.identifier("join")), [t.cloneNode(node.arguments[1])]));
        return;
      }
      if (node.arguments.some(t.isSpreadElement)) {
        const applyArgs = callArgsArray(node.arguments);
        if (t.isMemberExpression(node.callee)) {
          const receiver = t.cloneNode(node.callee.object as t.Expression);
          const method = t.memberExpression(t.cloneNode(node.callee), t.identifier("apply"));
          path.replaceWith(t.callExpression(method, [receiver, applyArgs]));
        } else if (t.isExpression(node.callee)) {
          path.replaceWith(t.callExpression(t.memberExpression(t.cloneNode(node.callee), t.identifier("apply")), [t.nullLiteral(), applyArgs]));
        }
      }
    },
    ArrayExpression(path) {
      if (!path.node.elements.some(t.isSpreadElement)) return;
      path.replaceWith(arrayConcat(path.node.elements));
    },
    ObjectExpression(path) {
      if (!path.node.properties.some(t.isSpreadElement)) return;
      const args: t.Expression[] = [t.objectExpression([])];
      let pending: t.ObjectProperty[] = [];
      const flush = () => {
        if (pending.length) {
          args.push(t.objectExpression(pending));
          pending = [];
        }
      };
      for (const prop of path.node.properties) {
        if (t.isSpreadElement(prop)) {
          flush();
          args.push(t.cloneNode(prop.argument));
        } else if (t.isObjectProperty(prop)) {
          pending.push(t.cloneNode(prop));
        } else {
          throw new Error("node mirror cleanup: object methods with spread are not supported");
        }
      }
      flush();
      path.replaceWith(t.callExpression(t.memberExpression(t.identifier("Object"), t.identifier("assign")), args));
    },
  });
}

type Polyfill = "stringTrimStart" | "stringTrimEnd" | "stringReplaceAll" | "arrayAt";

function collectPolyfills(file: t.File): Set<Polyfill> {
  const out = new Set<Polyfill>();
  traverse(file, {
    MemberExpression(path) {
      if (path.node.computed || !t.isIdentifier(path.node.property)) return;
      if (path.node.property.name === "trimStart" || path.node.property.name === "trimLeft") out.add("stringTrimStart");
      else if (path.node.property.name === "trimEnd" || path.node.property.name === "trimRight") out.add("stringTrimEnd");
      else if (path.node.property.name === "replaceAll") out.add("stringReplaceAll");
      else if (path.node.property.name === "at") out.add("arrayAt");
    },
  });
  return out;
}

function polyfillPrelude(polyfills: Set<Polyfill>): string {
  if (polyfills.size === 0) return "";
  const lines = [
    "(function() {",
    "  function def(proto, name, fn) {",
    "    if (proto[name]) return;",
    "    Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true });",
    "  }",
  ];
  if (polyfills.has("stringTrimStart")) {
    lines.push(
      "  def(String.prototype, \"trimStart\", function() { return String(this).replace(/^[\\s\\uFEFF\\xA0]+/, \"\"); });",
      "  def(String.prototype, \"trimLeft\", String.prototype.trimStart);",
    );
  }
  if (polyfills.has("stringTrimEnd")) {
    lines.push(
      "  def(String.prototype, \"trimEnd\", function() { return String(this).replace(/[\\s\\uFEFF\\xA0]+$/, \"\"); });",
      "  def(String.prototype, \"trimRight\", String.prototype.trimEnd);",
    );
  }
  if (polyfills.has("stringReplaceAll")) {
    lines.push(
      "  def(String.prototype, \"replaceAll\", function(search, replacement) {",
      "    var source = String(this);",
      "    if (search instanceof RegExp) {",
      "      if (String(search.flags).indexOf(\"g\") === -1) throw new TypeError(\"String.prototype.replaceAll called with a non-global RegExp\");",
      "      return source.replace(search, replacement);",
      "    }",
      "    return source.split(String(search)).join(String(replacement));",
      "  });",
    );
  }
  if (polyfills.has("arrayAt")) {
    lines.push(
      "  def(Array.prototype, \"at\", function(index) {",
      "    var i = Math.trunc(index) || 0;",
      "    if (i < 0) i += this.length;",
      "    return this[i];",
      "  });",
      "  if (typeof Uint8Array !== \"undefined\") def(Uint8Array.prototype, \"at\", Array.prototype.at);",
    );
  }
  lines.push("})();", "");
  return lines.join("\n");
}

function exposeDefaultAsNamed(file: t.File): void {
  for (const node of file.program.body) {
    if (!t.isExportDefaultDeclaration(node)) continue;
    const decl = node.declaration;
    if (t.isFunctionDeclaration(decl) && decl.id) {
      file.program.body.push(t.exportNamedDeclaration(null, [
        t.exportSpecifier(t.identifier(decl.id.name), t.identifier("Default")),
      ]));
      return;
    }
    if (t.isClassDeclaration(decl) && decl.id) {
      file.program.body.push(t.exportNamedDeclaration(null, [
        t.exportSpecifier(t.identifier(decl.id.name), t.identifier("Default")),
      ]));
      return;
    }
    const local = t.identifier("Default");
    node.declaration = local;
    file.program.body.splice(file.program.body.indexOf(node), 0, t.variableDeclaration("const", [
      t.variableDeclarator(local, decl as t.Expression),
    ]));
    return;
  }
}

function atIndex(arg: t.Expression, obj: t.Expression): t.Expression {
  if (t.isUnaryExpression(arg, { operator: "-" }) && t.isNumericLiteral(arg.argument)) {
    return t.binaryExpression("-", t.memberExpression(obj, t.identifier("length")), t.numericLiteral(arg.argument.value));
  }
  return arg;
}

function callArgsArray(args: (t.Expression | t.SpreadElement | t.ArgumentPlaceholder)[]): t.Expression {
  if (args.length === 1 && t.isSpreadElement(args[0])) return t.cloneNode(args[0].argument);
  return arrayConcat(args.map((arg) => t.isSpreadElement(arg) ? arg : t.arrayExpression([t.cloneNode(arg as t.Expression)])));
}

function arrayConcat(elements: (t.Expression | t.SpreadElement | t.ArgumentPlaceholder | null)[]): t.CallExpression {
  const args = elements.map((el) => {
    if (!el) return t.arrayExpression([t.identifier("undefined")]);
    if (t.isSpreadElement(el)) return t.cloneNode(el.argument);
    if (t.isArgumentPlaceholder(el)) throw new Error("node mirror cleanup: argument placeholders are not supported");
    return t.arrayExpression([t.cloneNode(el)]);
  });
  return t.callExpression(t.memberExpression(t.arrayExpression([]), t.identifier("concat")), args);
}
