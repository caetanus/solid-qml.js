import * as t from "@babel/types";

export const CONTROL_TAGS = new Set(["Show", "For", "Index", "Switch", "Match", "Suspense", "Dynamic"]);
export const BUILTIN_TAGS = new Set(["Window", ...CONTROL_TAGS]);

export interface HCallParts {
  tag: t.Expression | t.PrivateName;
  props: t.Node | undefined;
  children: t.Node[];
}

export function isHCall(node: t.Node | null | undefined): node is t.CallExpression {
  return !!node && t.isCallExpression(node) && t.isIdentifier(node.callee, { name: "h" });
}

export function hParts(call: t.CallExpression): HCallParts {
  if (!isHCall(call)) throw new Error(`expected normalized h(...) call, got ${call.type}`);
  const [tag, props, ...children] = call.arguments;
  if (!tag || !t.isExpression(tag) && !t.isPrivateName(tag))
    throw new Error("normalized h(...) is missing a tag expression");
  return { tag, props, children };
}

export function isFragmentTag(tag: t.Node): boolean {
  return t.isIdentifier(tag, { name: "hFrag" })
    || (t.isMemberExpression(tag) && !tag.computed && t.isIdentifier(tag.property, { name: "Fragment" }));
}

export function isBuiltinIdentifier(tag: t.Node): boolean {
  return t.isIdentifier(tag) && BUILTIN_TAGS.has(tag.name);
}

export function isComponentIdentifier(tag: t.Node, known: Set<string>): tag is t.Identifier {
  return t.isIdentifier(tag) && known.has(tag.name) && !BUILTIN_TAGS.has(tag.name) && tag.name !== "h" && tag.name !== "hFrag";
}
