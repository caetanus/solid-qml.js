// Surface — a 3D surface graph in module solidqml.Widgets (owner directive 2026-07-06: charts + 3D,
// "use CssFill for QtGraphs", and "combine 3D and charts: walkerlake"). A CssFill "div" wrapper hosts
// a QtGraphs Surface3D as a foreign fill — the meeting point of 3D and data viz, exactly what the
// Walker Lake topography (Isaaks & Srivastava's canonical geostat dataset) is meant to show.
//
// Data path (pure-QML): Surface3D can't be populated from a JS array, so we drive it with a
// HeightMapSurfaceDataProxy reading a grayscale heightmap PNG. The canonical Walker Lake CSV
// (columns X,Y,V) is rasterized to that PNG by scripts/csv-to-heightmap.py; the bundled default is a
// placeholder basin until the real grid is dropped in. QtGraphs loads as a runtime plugin — no relink.
//
// The transpiler emits:  W.Surface { cssClass: […]; [heightMap]; [surfaceColor] }
import QtQuick
import QtGraphs
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property url heightMap: Qt.resolvedUrl("walkerlake.png")
    property color surfaceColor: "#41cd52"

    cssPrimitive: "div"
    implicitWidth: 360
    implicitHeight: 300

    Surface3D {
        anchors.fill: parent
        cameraZoomLevel: 120
        cameraPreset: Graphs3D.CameraPreset.IsometricRight

        Surface3DSeries {
            drawMode: Surface3DSeries.DrawSurfaceAndWireframe
            shading: Surface3DSeries.Shading.Smooth
            baseColor: root.surfaceColor
            wireframeColor: "#0d3b16"
            HeightMapSurfaceDataProxy {
                // heightMapFile is a plain filesystem path; strip the resolved url's file:// scheme.
                heightMapFile: root.heightMap.toString().replace(/^file:\/\//, "")
            }
        }
    }
}
