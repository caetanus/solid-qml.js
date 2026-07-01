import QtQuick
import QtQuick.Shapes

// One CSS background layer — a solid colour, a linear- or a radial-gradient — painted as a
// rounded rectangle matching its parent's size and the given (already clamped) corner radii.
// CssRect stacks these to render radial gradients and multi-layer `background` values that
// its single built-in gradient fill can't express. Like that fill, a layer paints OPAQUE and
// carries its alpha on the Shape's `opacity` (a translucent Shape fill blends wrong).
Shape {
    id: layer
    anchors.fill: parent
    preferredRendererType: Shape.CurveRenderer

    // { type: "color"|"linear"|"radial", color?, angle?, cx?, cy?, stops? } (from CssTheme).
    property var spec: ({})
    property var radii: [0, 0, 0, 0] // [topLeft, topRight, bottomRight, bottomLeft]

    readonly property string kind: (spec && spec.type) ? spec.type : "color"
    readonly property real peakAlpha: {
        if (layer.kind === "color")
            return (spec && spec.color) ? spec.color.a : 0
        if (!spec || !spec.stops)
            return 1
        var m = 0
        for (var i = 0; i < spec.stops.length; ++i)
            m = Math.max(m, spec.stops[i].color.a)
        return m
    }
    opacity: layer.peakAlpha

    function _r(i) { return (radii && radii[i] !== undefined) ? radii[i] : 0 }

    // Linear gradient line (CSS: 0deg points up, increasing clockwise).
    readonly property real _angleRad: ((layer.kind === "linear" && spec.angle !== undefined ? spec.angle : 180) * Math.PI) / 180
    readonly property real _dx: Math.sin(layer._angleRad)
    readonly property real _dy: -Math.cos(layer._angleRad)
    readonly property real _len: Math.abs(width * layer._dx) + Math.abs(height * layer._dy)

    ShapePath {
        strokeWidth: 0
        strokeColor: "transparent"
        fillColor: (layer.kind === "color" && layer.spec.color)
            ? Qt.rgba(layer.spec.color.r, layer.spec.color.g, layer.spec.color.b, 1.0)
            : "transparent"
        fillGradient: layer.kind === "linear" ? lin : (layer.kind === "radial" ? rad : null)

        startX: layer._r(0); startY: 0
        PathLine { x: Math.max(layer._r(0), layer.width - layer._r(1)); y: 0 }
        PathArc { x: layer.width; y: layer._r(1); radiusX: layer._r(1); radiusY: layer._r(1); direction: PathArc.Clockwise }
        PathLine { x: layer.width; y: Math.max(layer._r(1), layer.height - layer._r(2)) }
        PathArc { x: layer.width - layer._r(2); y: layer.height; radiusX: layer._r(2); radiusY: layer._r(2); direction: PathArc.Clockwise }
        PathLine { x: layer._r(3); y: layer.height }
        PathArc { x: 0; y: layer.height - layer._r(3); radiusX: layer._r(3); radiusY: layer._r(3); direction: PathArc.Clockwise }
        PathLine { x: 0; y: layer._r(0) }
        PathArc { x: layer._r(0); y: 0; radiusX: layer._r(0); radiusY: layer._r(0); direction: PathArc.Clockwise }
    }

    LinearGradient {
        id: lin
        x1: layer.width / 2 - layer._dx * layer._len / 2
        y1: layer.height / 2 - layer._dy * layer._len / 2
        x2: layer.width / 2 + layer._dx * layer._len / 2
        y2: layer.height / 2 + layer._dy * layer._len / 2
    }
    RadialGradient {
        id: rad
        readonly property real cxp: layer.width * (layer.spec.cx !== undefined ? layer.spec.cx : 0.5)
        readonly property real cyp: layer.height * (layer.spec.cy !== undefined ? layer.spec.cy : 0.5)
        centerX: rad.cxp
        centerY: rad.cyp
        focalX: rad.cxp
        focalY: rad.cyp
        // CSS default radial size = farthest-corner: distance from centre to the furthest box corner.
        centerRadius: Math.sqrt(Math.pow(Math.max(rad.cxp, layer.width - rad.cxp), 2)
                              + Math.pow(Math.max(rad.cyp, layer.height - rad.cyp), 2))
    }

    Component { id: stopComponent; GradientStop {} }

    // Stops are built imperatively (a declarative dynamic stop list is awkward), opaque — the
    // layer's alpha is carried on `opacity` (peakAlpha).
    function rebuildStops() {
        if (layer.kind !== "linear" && layer.kind !== "radial")
            return
        var g = layer.kind === "linear" ? lin : rad
        var built = []
        var stops = (layer.spec && layer.spec.stops) ? layer.spec.stops : []
        for (var i = 0; i < stops.length; ++i) {
            var c = stops[i].color
            built.push(stopComponent.createObject(g, { position: stops[i].position, color: Qt.rgba(c.r, c.g, c.b, 1.0) }))
        }
        g.stops = built
    }
    onSpecChanged: rebuildStops()
    Component.onCompleted: rebuildStops()
}
