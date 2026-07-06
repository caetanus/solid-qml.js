// Surface — a 3D surface graph in its own opt-in module solidqml.Widgets.Surface (owner directives
// 2026-07-06: charts + 3D; "use CssFill for QtGraphs"; "combine 3D and charts: walkerlake"; the graph
// must respond to the mouse and carry a heat-map + value scale + a reset). A CssFill "div" wrapper
// hosts a QtGraphs Surface3D as a foreign fill — the meeting point of 3D and data viz.
//
// Data: the bundled walkerlake.png is the REAL canonical grid — the 260×300 Walker Lake exhaustive V
// variable (Isaaks & Srivastava, via R's gstat walker.exh), rasterized by scripts/csv-to-heightmap.py
// from the extracted X,Y,V CSV. HeightMapSurfaceDataProxy maps the 0..255 image to the true V range
// [valueMin,valueMax] so the axis and legend read real values. The surface is coloured by height
// (RangeGradient heat-map); rotate/zoom/select are built into Surface3D; a Reset restores the view.
//
// The transpiler emits:  WSurface.Surface { cssClass: […]; [heightMap]; [valueMin]; [valueMax] }
import QtQuick
import QtGraphs
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property url heightMap: Qt.resolvedUrl("walkerlake.png")
    property real valueMin: 0
    property real valueMax: 1631   // Walker Lake V range

    cssPrimitive: "div"
    implicitWidth: 360
    implicitHeight: 300

    // Default camera pose, captured so Reset can restore it.
    readonly property real _zoom0: 120
    readonly property real _xrot0: 22
    readonly property real _yrot0: 45

    Surface3D {
        id: surf
        anchors.fill: parent
        cameraZoomLevel: root._zoom0
        cameraXRotation: root._xrot0
        cameraYRotation: root._yrot0

        // Heat-map: colour the surface by height instead of a flat fill, so the scale means something.
        theme: GraphsTheme {
            colorScheme: GraphsTheme.ColorScheme.Dark
            colorStyle: GraphsTheme.ColorStyle.RangeGradient
        }

        Surface3DSeries {
            drawMode: Surface3DSeries.DrawSurfaceAndWireframe
            shading: Surface3DSeries.Shading.Smooth
            wireframeColor: "#20304050"
            baseGradient: Gradient {
                GradientStop { position: 0.00; color: "#0b3d91" }
                GradientStop { position: 0.30; color: "#159c8f" }
                GradientStop { position: 0.55; color: "#41cd52" }
                GradientStop { position: 0.78; color: "#ffd23f" }
                GradientStop { position: 1.00; color: "#c1121f" }
            }
            HeightMapSurfaceDataProxy {
                heightMapFile: root.heightMap.toString().replace(/^file:\/\//, "")
                minYValue: root.valueMin
                maxYValue: root.valueMax
            }
        }
    }

    // Colour scale (heat-map legend): a gradient bar with the min/max V values.
    Column {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 4
        Text { text: root.valueMax.toFixed(0); color: "#cfe0ee"; font.pixelSize: 11 }
        Rectangle {
            width: 14; height: 120; radius: 2
            border.color: "#33ffffff"; border.width: 1
            gradient: Gradient {
                GradientStop { position: 0.00; color: "#c1121f" }
                GradientStop { position: 0.22; color: "#ffd23f" }
                GradientStop { position: 0.45; color: "#41cd52" }
                GradientStop { position: 0.70; color: "#159c8f" }
                GradientStop { position: 1.00; color: "#0b3d91" }
            }
        }
        Text { text: root.valueMin.toFixed(0); color: "#cfe0ee"; font.pixelSize: 11 }
        Text { text: "V"; color: "#8aa0b4"; font.pixelSize: 11 }
    }

    // Reset the camera to its default pose.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 12
        width: resetLabel.implicitWidth + 20; height: 26; radius: 5
        color: resetMa.containsMouse ? "#2a3a4a" : "#1c2a38"
        border.color: "#41cd52"; border.width: 1
        Text { id: resetLabel; anchors.centerIn: parent; text: "Reset view"; color: "#cfe0ee"; font.pixelSize: 12 }
        MouseArea {
            id: resetMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                surf.cameraZoomLevel = root._zoom0;
                surf.cameraXRotation = root._xrot0;
                surf.cameraYRotation = root._yrot0;
                surf.cameraTargetPosition = Qt.vector3d(0, 0, 0);
            }
        }
    }
}
