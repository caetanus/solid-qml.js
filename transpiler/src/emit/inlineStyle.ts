// Inline `style` on a CSS-engine box (<div>, <span>, <text>, … → W.Div / W.Text) → the engine's
// `inlineStyle` property (CssBox::inlineStyle, vendor/qml-css-engine). The engine overlays it on the
// cascade after :hover, so it beats every stylesheet rule, as on the web.
//
//  - style="color: red"      → `inlineStyle: "color: red"` — the engine's own declaration parser.
//  - style={{ a: …, … }}     → `inlineStyle: ({ "kebab-key": value, … })`: camelCase keys become
//    kebab-case and numeric literals on length properties get "px" (React DOM's rule) at compile
//    time; a dynamic value stays a reactive binding (a number it yields gets px in the engine).
//  - style={expr}            → `inlineStyle: expr` — a string or an object computed at runtime; the
//    engine's map setter does the same key/number conversion (the small runtime helper).
import * as t from "@babel/types";
import { emitExpr, type Scope } from "./expr.ts";

// React DOM's unitless properties (kebab-case) — the same set as the engine's isUnitlessProperty().
const UNITLESS = new Set([
  "animation-iteration-count", "aspect-ratio", "border-image-outset", "border-image-slice", "border-image-width",
  "column-count", "columns", "flex", "flex-grow", "flex-shrink", "flex-order", "flex-positive", "flex-negative",
  "grid-area", "grid-row", "grid-row-end", "grid-row-span", "grid-row-start", "grid-column", "grid-column-end",
  "grid-column-span", "grid-column-start", "font-weight", "line-clamp", "line-height", "opacity", "order",
  "orphans", "scale", "tab-size", "widows", "z-index", "zoom", "fill-opacity", "flood-opacity", "stop-opacity",
  "stroke-dasharray", "stroke-dashoffset", "stroke-miterlimit", "stroke-opacity", "stroke-width",
]);

/** `backgroundColor` → `background-color`, `WebkitTransform` → `-webkit-transform`, `msTransform` →
 *  `-ms-transform`; kebab-case and custom properties (`--x`) pass through. */
export function kebabCssProperty(key: string): string {
  if (key.startsWith("--")) return key;
  const prefixed = /^ms[A-Z]/.test(key) ? "-" + key : key;
  return prefixed.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase()).toLowerCase();
}

/** A numeric literal as the CSS value React DOM would write for `prop`. */
export function cssNumber(prop: string, n: number): string {
  return n === 0 || prop.startsWith("--") || UNITLESS.has(prop) ? String(n) : `${n}px`;
}

function numericLiteral(v: t.Node): number | null {
  if (t.isNumericLiteral(v)) return v.value;
  if (t.isUnaryExpression(v) && v.operator === "-" && t.isNumericLiteral(v.argument)) return -v.argument.value;
  return null;
}

/** The value of `inlineStyle:` for a `style` attribute value. */
export function inlineStyleValue(v: t.Node, scope: Scope): string {
  if (t.isStringLiteral(v)) return JSON.stringify(v.value);
  if (t.isTemplateLiteral(v) && v.expressions.length === 0) return JSON.stringify(v.quasis[0].value.cooked ?? "");
  const binding = { ...scope, mode: "binding" as const };
  // A literal object whose keys are all known at compile time: converted here. Anything else (a spread,
  // a computed key) is an object built at runtime — the engine converts it.
  if (t.isObjectExpression(v) && v.properties.every((p) => t.isObjectProperty(p) && !p.computed
      && (t.isIdentifier(p.key) || t.isStringLiteral(p.key)))) {
    const entries = (v.properties as t.ObjectProperty[]).map((p) => {
      const prop = kebabCssProperty(t.isIdentifier(p.key) ? p.key.name : (p.key as t.StringLiteral).value);
      const n = numericLiteral(p.value);
      let value: string;
      if (n !== null) value = JSON.stringify(cssNumber(prop, n));
      else if (t.isStringLiteral(p.value)) value = JSON.stringify(p.value.value);
      else if (t.isExpression(p.value)) value = emitExpr(p.value, binding);
      else throw new Error(`style: unsupported value for \`${prop}\``);
      return `${JSON.stringify(prop)}: ${value}`;
    });
    return `({ ${entries.join(", ")} })`;
  }
  if (!t.isExpression(v)) throw new Error("style must be a string or an object");
  return emitExpr(v, binding);
}

/** The `inlineStyle:` line for an element's `style` attribute (none without one). */
export function buildInlineStyleLine(style: t.Node | undefined, scope: Scope, pad: string): string[] {
  return style ? [`${pad}inlineStyle: ${inlineStyleValue(style, scope)}`] : [];
}
