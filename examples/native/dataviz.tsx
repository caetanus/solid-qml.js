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

declare const Chart: any, Scene3D: any, Surface: any, MediaPlayer: any, WebView: any, RichText: any;

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
        {/* Complex 2D plot (module solidqml.Widgets.Chart): a filled area, two splines and a scatter
            overlay share value axes, with hover tooltips. Bars are intentionally absent — those we
            draw with the CSS engine; QtGraphs earns its place only for the hard, multi-series stuff. */}
        <div class="dv-card">
          <text class="dv-label">Complex plot — area · splines · scatter, hover for values (QtGraphs)</text>
          <Chart class="dv-chart" />
        </div>
        {/* Real-time 3D (module solidqml.Widgets.Scene3D): the Blender monkey (Suzanne) imported via
            balsam to a .mesh, under a three-point light rig, slowly spinning on a GPU surface. */}
        <div class="dv-card">
          <text class="dv-label">Blender monkey · three-point lighting, spinning (Qt Quick 3D)</text>
          <Scene3D class="dv-scene" />
        </div>
        {/* Media playback (module solidqml.Widgets.Media): QtMultimedia, imported only when the
            tag is used. A generated clip autoplays as the demo's visualization: moving test-pattern
            video with the play/seek/clock strip below. */}
        <div class="dv-card">
          <text class="dv-label">Media player — video + play/seek/clock (QtMultimedia)</text>
          <MediaPlayer class="dv-media" src="assets/media-demo.mp4" autoplay />
        </div>
        {/* Embedded web content (module solidqml.Widgets.Web): a REAL Chromium page via
            QtWebEngine, imported only when the tag is used (the loader pre-sets
            AA_ShareOpenGLContexts, dependency-free). Local HTML asset — no network needed. */}
        <div class="dv-card">
          <text class="dv-label">WebView — Chromium in a card (QtWebEngine)</text>
          <WebView class="dv-web" src="assets/web-demo.html" />
        </div>
        {/* Word-like editor (module solidqml.Widgets.RichText): character/paragraph formatting
            via the loader's QTextCursor handler, saving OpenDocument NATIVELY
            (QTextDocumentWriter) and loading .odt through our content.xml reader. */}
        <div class="dv-card">
          <text class="dv-label">Rich text — word-like editing, opens &amp; saves OpenDocument (.odt)</text>
          <RichText class="dv-rich" />
        </div>
        {/* 3D + data viz combined (module solidqml.Widgets.Surface): the REAL Walker Lake exhaustive
            grid (Isaaks & Srivastava, 260×300 V values) as a height-coloured surface. Drag to rotate,
            wheel to zoom, click to select (Surface3D built-ins); the scale legend reads real V, and
            Reset restores the view. */}
        <div class="dv-card">
          <text class="dv-label">Walker Lake topography — heat-mapped 3D surface, mouse-driven (QtGraphs Surface3D)</text>
          <Surface class="dv-surface" />
        </div>
      </Show>
    </div>
  );
}
