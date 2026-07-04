// Native-only tag registry (plan: 2026-07-03-native-only-widgets, phases 2–3).
//
// Each module in this directory implements a GROUP of native tags/mappings and registers
// them here at import time. qml.ts consults the registry in BOTH dispatch paths (lowercase
// HTML string tags and Capitalized builtin tags) BEFORE user components, so registered
// tags shadow user components exactly like <Calendar> does.
//
// An emitter receives the raw h() pieces and the live Scope; helpers (emitChildren,
// buildCssClassLine, guardLine, …) are imported from "../qml.ts" — the import cycle is
// benign because everything is called at emit time, never at module evaluation.
import type * as t from "@babel/types";
import type { Scope } from "../expr.ts";

export type NativeEmit = (
  propsArg: t.Node | undefined,
  children: t.Node[],
  scope: Scope,
  level: number,
  guard: string | undefined,
) => string[];

// Cycle-safe registry storage: the group modules call registerNativeTags DURING the import
// cycle (qml.ts → index.ts → group module → registerNativeTags), i.e. BEFORE this module's
// body has run — a `const` map would still be in its temporal dead zone at that point.
// `var` hoists to undefined (no TDZ) and the hoisted accessor lazily creates the map.
var _nativeTags: Map<string, NativeEmit> | undefined;
function tags(): Map<string, NativeEmit> {
  return (_nativeTags ??= new Map<string, NativeEmit>());
}

export const nativeTags = tags();

export function registerNativeTags(entries: Record<string, NativeEmit>): void {
  for (const [name, fn] of Object.entries(entries)) {
    if (tags().has(name)) throw new Error(`native tag registered twice: ${name}`);
    tags().set(name, fn);
  }
}

/** Request an extra QML import line for the CURRENT component (e.g. Qt.labs.platform).
 *  Deduped; emitted by emitComponentType next to the Templates import. */
export function requireImport(scope: Scope, importLine: string): void {
  if (!scope.usedWidgets) return;
  (scope.usedWidgets.extraImports ??= new Set()).add(importLine);
}

// Group modules (side-effect registration). Each is owned by one implementation task:
import "./htmlwidgets.ts"; // <progress>, <fieldset>/<legend>, title→ToolTip, <dialog>, <details>/<summary>
import "./containers.ts";  // <ToolBar>, <TabBar>/<TabButton>, <SplitView>, <Drawer>, <StackView>, <SwipeView>/<PageIndicator>
import "./inputs.ts";      // <RangeSlider>, <Dial>, <Tumbler>, <DelayButton>, <BusyIndicator>, <RoundButton>, <ToolButton>, <ToolSeparator>
import "./menus.ts";       // <Menu>/<MenuItem>/<MenuSeparator>/<MenuBar>, oncontextmenu, <TreeView>, <Tray>
