// QML property/id names that collide with built-ins or QML keywords. A signal/prop named `visible`
// must be emitted as `visible_` so it doesn't fight the real QML property. ONE place owns this.
const RESERVED = new Set([
  "visible", "width", "height", "x", "y", "z", "opacity", "scale", "rotation", "state", "states",
  "parent", "children", "data", "color", "enabled", "focus", "clip", "anchors", "item", "index",
  "property", "function", "signal", "import", "as", "on", "default", "readonly", "true", "false",
  "null", "var", "int", "real", "string", "bool", "id",
]);

/** The single source of truth for any author identifier emitted into QML. */
export function safeName(name: string): string {
  if (RESERVED.has(name)) return `${name}_`;
  if (!/^[A-Za-z_]\w*$/.test(name)) return `_${name.replace(/[^\w]/g, "_")}`;
  return name;
}
