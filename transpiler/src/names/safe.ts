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

/** Prefix of a component member whose author name starts upper-case (see `memberName`). Its first
 *  LETTER is lower-case on purpose: Qt derives a change handler by upper-casing the first letter
 *  (`on__Uc_XChanged`) and resolves it back by lower-casing it, so a name whose first letter is
 *  upper-case (`__X`) would have no reachable `on…Changed` handler at all. No native QML property
 *  starts with `__`, and no transpiler-internal name starts with `__uc_`. */
const UPPER_PREFIX = "__uc_";

/** An author identifier that the component DECLARES on its QML object — a property (signal, memo,
 *  store, prop, promoted const/let, resource, context member) or a method (helper). QML rejects a
 *  property, method or signal name whose first character is upper-case ("Property names cannot
 *  begin with an upper case letter" — the whole type fails to load), so `SIZE_STEPS` becomes
 *  `__uc_SIZE_STEPS`; every other name is exactly `safeName`. Declarations and every reference to
 *  the member go through this ONE function. */
export function memberName(name: string): string {
  const s = safeName(name);
  return /^[A-Z]/.test(s) ? `${UPPER_PREFIX}${s}` : s;
}

/** The QML change-handler name for a property (`count` → `onCountChanged`, `__uc_X` →
 *  `on__Uc_XChanged`): Qt upper-cases the first LETTER after `on`, skipping leading `_`/`$`
 *  (QQmlSignalNames::signalNameToHandlerName). */
export function changedHandlerName(prop: string): string {
  const i = prop.search(/[A-Za-z]/);
  const cased = i < 0 ? prop : prop.slice(0, i) + prop.charAt(i).toUpperCase() + prop.slice(i + 1);
  return `on${cased}Changed`;
}
