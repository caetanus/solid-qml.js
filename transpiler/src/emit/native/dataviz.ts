// Data-viz group: <Chart> (QtGraphs bar chart) and <Scene3D> (QtQuick3D viewport).
//
// Owner directive 2026-07-06: charts + 3D go in the demo, wrapped in a CssFill (so they lay out
// via the CSS engine) hosting the Qt module's view as a foreign fill. The widget SHELLS live in
// qml/solidqml/Widgets/{Chart,Scene3D}.qml; this emit is thin — it wires the author classes and a
// few props (the QtGraphs series / the 3D model+lighting live in the .qml). Both Qt modules load as
// runtime plugins, so the loader needs no relink.
import * as t from "@babel/types";
import { registerNativeTags, requireImport } from "./index.ts";
import { emitExpr, type Scope } from "../expr.ts";
import { buildCssClassLine, INDENT } from "../qml.ts";

// Each viz widget is its OWN opt-in QML module (owner directive 2026-07-06: "import
// Solid.Widgets.Surface"; lazy imports — some Qt modules won't resolve on every machine). The core
// solidqml.Widgets stays free of QtGraphs/QtQuick3D; the heavy import lives only in the per-widget
// module, and the transpiler pulls it in ONLY when the tag is used (so an app that never uses <Chart>
// never imports QtGraphs). requireImport adds the import line next to the component's other imports.

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

/** <Chart accent="#…" xMax={n}> → WChart.Chart (opt-in module; complex multi-series QtGraphs plot). */
function emitChart(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  requireImport(scope, "import solidqml.Widgets.Chart 1.0 as WChart");
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const lines: string[] = [`${pad}WChart.Chart {`, ...buildCssClassLine(cssPropsShim(props), scope, i(1))];
  if (guard) lines.push(`${i(1)}visible: !!(${guard})`);
  const accent = props.get("accent");
  const xMax = props.get("xMax");
  if (accent) lines.push(`${i(1)}accent: ${bind(accent)}`);
  if (xMax) lines.push(`${i(1)}xMax: ${bind(xMax)}`);
  lines.push(`${pad}}`);
  return lines;
}

/** <Scene3D modelColor="#…" spinning={bool}> → WScene3D.Scene3D (opt-in module; View3D + Suzanne). */
function emitScene3D(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  requireImport(scope, "import solidqml.Widgets.Scene3D 1.0 as WScene3D");
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const lines: string[] = [`${pad}WScene3D.Scene3D {`, ...buildCssClassLine(cssPropsShim(props), scope, i(1))];
  if (guard) lines.push(`${i(1)}visible: !!(${guard})`);
  const modelColor = props.get("modelColor");
  const spinning = props.get("spinning");
  if (modelColor) lines.push(`${i(1)}modelColor: ${bind(modelColor)}`);
  if (spinning) lines.push(`${i(1)}spinning: ${bind(spinning)}`);
  lines.push(`${pad}}`);
  return lines;
}

/** <Surface heightMap="…" surfaceColor="#…"> → W.Surface (CssFill + QtGraphs Surface3D). The
 *  Walker Lake topography: 3D + charts combined, driven by a heightmap rasterized from the CSV. */
function emitSurface(propsArg: t.Node | undefined, _children: t.Node[], scope: Scope, level: number, guard?: string): string[] {
  const pad = INDENT.repeat(level);
  const i = (n: number) => INDENT.repeat(level + n);
  requireImport(scope, "import solidqml.Widgets.Surface 1.0 as WSurface");
  const props = propsOf(propsArg);
  const bind = (e: t.Expression) => emitExpr(e, { ...scope, mode: "binding" });

  const lines: string[] = [`${pad}WSurface.Surface {`, ...buildCssClassLine(cssPropsShim(props), scope, i(1))];
  if (guard) lines.push(`${i(1)}visible: !!(${guard})`);
  const heightMap = props.get("heightMap");
  const valueMin = props.get("valueMin");
  const valueMax = props.get("valueMax");
  if (heightMap) lines.push(`${i(1)}heightMap: ${bind(heightMap)}`);
  if (valueMin) lines.push(`${i(1)}valueMin: ${bind(valueMin)}`);
  if (valueMax) lines.push(`${i(1)}valueMax: ${bind(valueMax)}`);
  lines.push(`${pad}}`);
  return lines;
}

registerNativeTags({ Chart: emitChart, Scene3D: emitScene3D, Surface: emitSurface });
