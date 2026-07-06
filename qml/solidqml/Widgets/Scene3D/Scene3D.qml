// Scene3D — a Qt Quick 3D viewport in its own opt-in module solidqml.Widgets.Scene3D (owner
// directives 2026-07-06: "use CssFill for QtQuick3D"; the monkey must respond to the mouse, have
// better lighting, and a background). A CssFill "div" wrapper hosts a gradient backdrop with a
// TRANSPARENT View3D on top, so the scene floats over the gradient. The showcase model is the Blender
// monkey (Suzanne, bundled suzanne.mesh from balsam).
//
//  - Mouse: an OrbitCameraController (drag to orbit, wheel to zoom) — View3D has no built-in controls.
//  - Framing: the imported mesh is neither centred nor unit-sized, so we AUTO-FIT from Model.bounds —
//    a pivot Node at the origin carries the idle spin and the Model is scaled + centred onto it, so it
//    turns in place, fully framed, whatever the raw coordinates.
//  - Lighting: a bright key, a cool fill and a warm rim, plus a soft ambient, for shape and mood.
//
// The transpiler emits:  WScene3D.Scene3D { cssClass: […]; [modelColor]; [spinning] }
import QtQuick
import QtQuick3D
import QtQuick3D.Helpers
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property color modelColor: "#41cd52"
    property bool spinning: true
    property real fitSize: 3.2
    property real orbitRadius: 0.6   // small offset from the pivot → a gentle orbit, not a big swing

    cssPrimitive: "div"
    implicitWidth: 320
    implicitHeight: 260

    function fit() {
        var b = monkey.bounds;
        if (!b)
            return;
        var mn = b.minimum, mx = b.maximum;
        var dim = Math.max(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
        if (dim <= 0)
            return;
        var s = root.fitSize / dim;
        monkey.scale = Qt.vector3d(s, s, s);
        // Centre the mesh on the pivot, then nudge it by orbitRadius so the pivot's spin orbits it
        // in a small circle (keeps the orbit the owner likes without swinging into the camera).
        monkey.position = Qt.vector3d(-(mn.x + mx.x) / 2 * s + root.orbitRadius, -(mn.y + mx.y) / 2 * s, -(mn.z + mx.z) / 2 * s);
    }

    // Gradient backdrop (shows through the transparent View3D).
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#241a4a" }
            GradientStop { position: 0.55; color: "#181022" }
            GradientStop { position: 1.0; color: "#0c1420" }
        }
    }

    View3D {
        id: view
        anchors.fill: parent

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Transparent
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        // Camera on an origin node so the OrbitCameraController can orbit it with the mouse.
        Node {
            id: camOrigin
            PerspectiveCamera {
                id: cam
                position: Qt.vector3d(0, 0, 6)
                clipNear: 0.1
                clipFar: 1000
            }
        }
        OrbitCameraController {
            origin: camOrigin
            camera: cam
        }

        // Three-point rig + a soft ambient fill.
        DirectionalLight { eulerRotation.x: -30; eulerRotation.y: -35; brightness: 1.5 }
        DirectionalLight { eulerRotation.x: 10; eulerRotation.y: 150; color: "#8fbcff"; brightness: 0.6 }
        PointLight { position: Qt.vector3d(-280, 180, 260); color: "#7cc0ff"; brightness: 6 }
        PointLight { position: Qt.vector3d(260, -80, 160); color: "#ffb36b"; brightness: 5 }
        PointLight { position: Qt.vector3d(0, 40, -320); color: "#ff80c0"; brightness: 4 }

        // Pivot at the origin carries the idle spin; the centred Model turns in place.
        Node {
            id: pivot
            eulerRotation.x: 12
            NumberAnimation on eulerRotation.y {
                running: root.spinning
                from: 0; to: 360
                duration: 12000
                loops: Animation.Infinite
            }
            Model {
                id: monkey
                source: "suzanne.mesh"
                onBoundsChanged: root.fit()
                Component.onCompleted: root.fit()
                materials: PrincipledMaterial {
                    baseColor: root.modelColor
                    metalness: 0.3
                    roughness: 0.3
                }
            }
        }
    }
}
