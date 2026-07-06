// Scene3D — a Qt Quick 3D viewport in its own opt-in module solidqml.Widgets.Scene3D (owner
// directive 2026-07-06: "use CssFill for QtQuick3D"). A CssFill "div" wrapper (CSS layout/paint +
// author classes) hosts a View3D as a foreign fill. The showcase model is the Blender monkey
// (Suzanne, bundled suzanne.mesh from balsam), lit with a three-point rig and slowly spun.
//
// The imported mesh is neither centred on the origin nor unit-sized, which made it fill the frame
// off-centre. So we AUTO-FIT at runtime from Model.bounds: a pivot Node at the origin carries the
// spin, and the Model is scaled to a fixed size and translated so its centre sits on the pivot — it
// then spins in place, fully framed, whatever the raw mesh coordinates.
//
// The transpiler emits:  WScene3D.Scene3D { cssClass: […]; [modelColor]; [spinning] }
import QtQuick
import QtQuick3D
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property color modelColor: "#41cd52"
    property bool spinning: true
    property real fitSize: 3.2   // target width/height/depth after fitting

    cssPrimitive: "div"
    implicitWidth: 320
    implicitHeight: 260

    // Centre + scale the mesh from its bounds so it sits on the pivot origin at a known size.
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
        monkey.position = Qt.vector3d(-(mn.x + mx.x) / 2 * s, -(mn.y + mx.y) / 2 * s, -(mn.z + mx.z) / 2 * s);
    }

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#181022"
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        // Straight-on, pulled back so the fitSize-sized model sits comfortably inside the 60° FOV.
        PerspectiveCamera {
            position: Qt.vector3d(0, 0, 6)
            clipNear: 0.1
            clipFar: 1000
        }

        // Three-point lighting: a bright key, a cool fill, and a warm rim for shape and mood.
        DirectionalLight { eulerRotation.x: -35; eulerRotation.y: -30; brightness: 1.3 }
        PointLight { position: Qt.vector3d(-260, 160, 240); color: "#6fb7ff"; brightness: 4 }
        PointLight { position: Qt.vector3d(240, -60, 140); color: "#ffb36b"; brightness: 3 }

        // Pivot at the origin carries the spin; the centred Model turns in place.
        Node {
            id: pivot
            eulerRotation.x: 12
            NumberAnimation on eulerRotation.y {
                running: root.spinning
                from: 0; to: 360
                duration: 9000
                loops: Animation.Infinite
            }
            Model {
                id: monkey
                source: "suzanne.mesh"
                onBoundsChanged: root.fit()
                Component.onCompleted: root.fit()
                materials: PrincipledMaterial {
                    baseColor: root.modelColor
                    metalness: 0.25
                    roughness: 0.35
                }
            }
        }
    }
}
