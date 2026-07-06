// Data-viz group: <Chart> (QtGraphs bar chart) and <Scene3D> (QtQuick3D viewport).
//
// Owner directive 2026-07-06: charts + 3D go in the demo, wrapped in a CssFill (so they lay out
// via the CSS engine) hosting the Qt module's view as a foreign fill. The widget SHELLS live in
// qml/solidqml/Widgets/{Chart,Scene3D}.qml; this emit is thin — it wires the author classes and a
// few props (the QtGraphs series / the 3D model+lighting live in the .qml). Both Qt modules load as
// runtime plugins, so the loader needs no relink.
import * as t from "@babel/types";
import { registerNativeTags } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { buildCssClassLine, INDENT } from "../qml.ts";

function propsOf(propsArg: t.Node | undefined): Map<string, t.Expression> {
  const map = new Map<string, t.Expression>();
  if (!propsArg || !t.isObjectExpression(propsArg)) return map;
  for (const p of propsArg.properties) {
    if (!t.isObjectProperty(p) || !t.isIdentifier(p.key) || !t.isExpression(p.value)) continue;
    map.set(p.key.name, p.value);
  }
  return map;
}

function classesOf(props: Map<string, t.Expression>): string[] {
  const c = props.get("class");
  return c && t.isStringLiteral(c) ? c.value.split(/\s+/).filter(Boolean) : [];
}

function cssPropsShim(props: Map<string, t.Expression>) {
  return { classes: classesOf(props), classList: [], onClick: undefined, ref: undefined, draggable: false, dragData: undefined, onDrop: undefined };
}

function markWidgetLib(scope: Scope): void {
  if (scope.usedWidgets) scope.usedWidgets.widgetLib = true;
}

/** <Chart categories={[…]} values={[…]} barColor="#…" axisMax={n}> → W.Chart (CssFill + GraphsView). */
function emitChart(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const lines: string[] = [`${pad}W.Chart {`, ...buildCssClassLine(cssPropsShim(props), scope, i(1))];
  if (guard) lines.push(`${i(1)}visible: !!(${guard})`);
  const categories = props.get("categories");
  const values = props.get("values");
  const barColor = props.get("barColor");
  const axisMax = props.get("axisMax");
  if (categories) lines.push(`${i(1)}categories: ${bind(categories)}`);
  if (values) lines.push(`${i(1)}values: ${bind(values)}`);
  if (barColor) lines.push(`${i(1)}barColor: ${bind(barColor)}`);
  if (axisMax) lines.push(`${i(1)}axisMax: ${bind(axisMax)}`);
  lines.push(`${pad}}`);
  return lines;
}

/** <Scene3D modelColor="#…" spinning={bool}> → W.Scene3D (CssFill + View3D + the Suzanne model). */
function emitScene3D(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  markWidgetLib(scope);
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const lines: string[] = [`${pad}W.Scene3D {`, ...buildCssClassLine(cssPropsShim(props), scope, i(1))];
  if (guard) lines.push(`${i(1)}visible: !!(${guard})`);
  const modelColor = props.get("modelColor");
  const spinning = props.get("spinning");
  if (modelColor) lines.push(`${i(1)}modelColor: ${bind(modelColor)}`);
  if (spinning) lines.push(`${i(1)}spinning: ${bind(spinning)}`);
  lines.push(`${pad}}`);
  return lines;
}

registerNativeTags({ Chart: emitChart, Scene3D: emitScene3D });
