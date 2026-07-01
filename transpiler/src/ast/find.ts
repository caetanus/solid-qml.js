import _traverse from "@babel/traverse";
import * as t from "@babel/types";
import type { Node } from "@babel/types";
import { isHCall } from "./h.ts";

// @babel/traverse ships CJS; under ESM the function is on `.default`.
const traverse: typeof _traverse = (_traverse as any).default ?? _traverse;

/**
 * Find the render of the first exported component: the `h(...)` CallExpression returned by an
 * exported function. Returns null if it returns no h() call. (Multi-component handling is a later
 * plan; this walking skeleton assumes one exported component.)
 */
export function findRender(ast: Node | null | undefined): t.CallExpression | null {
  if (!ast) return null;
  let render: t.CallExpression | null = null;

  traverse(ast as t.File, {
    ReturnStatement(path) {
      if (render) return;
      const arg = path.node.argument;
      if (isHCall(arg)) {
        // Only the return inside an exported function (skip nested returns for now).
        const fn = path.getFunctionParent();
        if (fn && isExported(fn)) render = arg;
      }
    },
  });
  return render;
}

function isExported(fnPath: { node: t.Node; parentPath: any }): boolean {
  let p: any = fnPath;
  while (p) {
    if (t.isExportNamedDeclaration(p.node) || t.isExportDefaultDeclaration(p.node)) return true;
    p = p.parentPath;
  }
  return false;
}
