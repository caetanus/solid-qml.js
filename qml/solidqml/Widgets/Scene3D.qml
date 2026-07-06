// Scene3D — a Qt Quick 3D viewport in module solidqml.Widgets (owner directive 2026-07-06: "use
// CssFill for QtQuick3D"). A CssFill "div" wrapper (CSS layout/paint + author classes) hosts a
// View3D as a foreign fill. The showcase model is the Blender monkey (Suzanne), imported via balsam
// to qml/solidqml/assets/suzanne.mesh, lit with a three-point rig and slowly spun — QtQuick3D loads
// as a runtime plugin, so the loader needs no relink.
//
// The transpiler emits:  W.Scene3D { cssClass: […]; [modelColor]; [spinning] }
import QtQuick
import QtQuick3D
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property color modelColor: "#41cd52"
    property bool spinning: true

    cssPrimitive: "div"
    implicitWidth: 320
    implicitHeight: 260

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#181022"
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        PerspectiveCamera {
            position: Qt.vector3d(0, 1.2, 5.2)
            eulerRotation.x: -8
        }

        // Three-point lighting: a bright key, a cool fill, and a warm rim for shape and mood.
        DirectionalLight {
            eulerRotation.x: -35
            eulerRotation.y: -30
            brightness: 1.3
        }
        PointLight {
            position: Qt.vector3d(-260, 160, 240)
            color: "#6fb7ff"
            brightness: 4
        }
        PointLight {
            position: Qt.vector3d(240, -60, 140)
            color: "#ffb36b"
            brightness: 3
        }

        Model {
            id: monkey
            source: "../assets/suzanne.mesh"
            scale: Qt.vector3d(1.35, 1.35, 1.35)
            eulerRotation.x: 12
            materials: PrincipledMaterial {
                baseColor: root.modelColor
                metalness: 0.25
                roughness: 0.35
            }
            NumberAnimation on eulerRotation.y {
                running: root.spinning
                from: 0; to: 360
                duration: 9000
                loops: Animation.Infinite
            }
        }
    }
}
