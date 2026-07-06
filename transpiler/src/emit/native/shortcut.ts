// <Shortcut> — an app-level keyboard shortcut / accelerator (owner request 2026-07-06: "adicione
// <QShortcut> ou <QAccelerator> pra gente"). Maps to QML's Shortcut type (which wraps the platform
// QShortcut), so authors bind key sequences declaratively:
//
//   <Shortcut keys="Ctrl+S" onActivated={() => save()} />
//   <Shortcut keys={["Ctrl+Q", "Ctrl+W"]} onActivated={quit} />
//
// keys is a single sequence string or an array of them; a <Show> guard folds into `enabled`. No
// visual — it just installs the accelerator wherever it sits in the scene.
import * as t from "@babel/types";
import { registerNativeTags } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { INDENT } from "../qml.ts";
import { safeName } from "../../names/safe.ts";

type Fn = t.ArrowFunctionExpression | t.FunctionExpression;

function propsOf(propsArg: t.Node | undefined): Map<string, t.Expression> {
  const map = new Map<string, t.Expression>();
  if (!propsArg || !t.isObjectExpression(propsArg)) return map;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
    map.set(p.key.name, p.value);
  }
  return map;
}

/** Author handler → QML statement body (inline arrow/function, or a bare helper identifier → call). */
function handlerBody(node: t.Node, scope: Scope): string {
  if (t.isArrowFunctionExpression(node) || t.isFunctionExpression(node)) {
    const fn = node as Fn;
    const inner: Scope = { ...scope, mode: "handler" };
    if (t.isBlockStatement(fn.body)) {
      return fn.body.body.map((s) => {
        if (t.isExpressionStatement(s)) return `${emitExpr(s.expression, inner)};`;
        if (t.isReturnStatement(s) && s.argument) return `return ${emitExpr(s.argument, inner)};`;
        return "";
      }).filter(Boolean).join(" ");
    }
    return emitExpr(fn.body, inner);
  }
  if (t.isIdentifier(node) && scope.helpers?.has(node.name)) return `${safeName(node.name)}()`;
  throw new Error("<Shortcut onActivated> takes an inline arrow/function or a helper reference");
}

function emitShortcut(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const keys = props.get("keys");
  let sequences = "[]";
  if (keys) {
    if (t.isStringLiteral(keys)) sequences = `[${JSON.stringify(keys.value)}]`;
    else if (t.isArrayExpression(keys)) sequences = bind(keys);
    else sequences = `[${bind(keys)}]`; // a dynamic single sequence
  }

  const lines = [`${pad}Shortcut {`, `${i(1)}sequences: ${sequences}`];
  if (guard) lines.push(`${i(1)}enabled: !!(${guard})`);
  const onActivated = props.get("onActivated");
  if (onActivated) lines.push(`${i(1)}onActivated: { ${handlerBody(onActivated, scope)} }`);
  lines.push(`${pad}}`);
  return lines;
}

registerNativeTags({ Shortcut: emitShortcut });
