// Charts & 3D page (native-only). <Chart>/<Scene3D>/<Surface> are registry-dispatched by the
// transpiler into their per-widget opt-in modules (solidqml.Widgets.Chart/.Scene3D/.Surface), each
// pulling QtGraphs/QtQuick3D lazily — so this page is the showcase for those optional modules. Like
// the other native views it self-gates behind the process.versions.solidQml probe (the browser build
// shows a fallback card; QtGraphs/QtQuick3D are QML-only surface).
//
// The Surface is the Walker Lake topography (Isaaks & Srivastava's canonical geostat grid): 3D and
// charts combined. Its heightmap is a placeholder basin until the canonical CSV is rasterized by
// scripts/csv-to-heightmap.py into qml/solidqml/Widgets/Surface/walkerlake.png.
import { Show } from "solid-js";
import { div, text } from "../../src/solid-qml/runtime";
import "./dataviz.css";

declare const Chart: any, Scene3D: any, Surface: any;

export function ChartsAnd3D() {
  const isNative = typeof process !== "undefined" && !!(process.versions && process.versions.solidQml);
  return (
    <div class="dv">
      <text class="dv-h">Charts &amp; 3D</text>
      <text class="dv-sub">Optional opt-in modules — QtGraphs &amp; Qt Quick 3D, imported only when used.</text>
      <Show
        when={isNative}
        fallback={
          <div class="dv-fallback">
            <text class="dv-fallback-t">charts &amp; 3D — run this view in the QML loader</text>
          </div>
        }
      >
        <div class="dv-card">
          <text class="dv-label">Complex plot — area · splines · scatter (QtGraphs)</text>
          <Chart class="dv-chart" />
        </div>
        <div class="dv-card">
          <text class="dv-label">Blender monkey · three-point lighting (Qt Quick 3D)</text>
          <Scene3D class="dv-scene" />
        </div>
        <div class="dv-card">
          <text class="dv-label">Walker Lake topography — 3D surface (QtGraphs Surface3D)</text>
          <Surface class="dv-surface" />
        </div>
      </Show>
    </div>
  );
}
