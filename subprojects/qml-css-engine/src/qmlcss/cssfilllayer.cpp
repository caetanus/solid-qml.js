#include "qmlcss/componentcache.h"
#include "qmlcss/cssfilllayer.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>

#include <algorithm>

// The composed render shell — a REAL QtQuick Shape (the original QML's root type) driven entirely
// by the inputs C++ pushes onto it. C++ manages geometry (no anchors in the shell); the shell's
// JS bindings faithfully reproduce the QML geometry expressions (gradient line, radial centre,
// ShapePath path points) that depend on width/height. Gradient stops are built imperatively by
// rebuildStops(), triggered by the shell's `onSpecChanged: rebuildStops()` — exactly as in the
// original QML. `Component.onCompleted: rebuildStops()` covers the initial-spec path.
static const char *kFillLayerShell = R"QML(
import QtQuick
import QtQuick.Shapes

Shape {
    id: layer
    preferredRendererType: Shape.CurveRenderer

    // --- inputs pushed by C++ -------------------------------------------------------
    property string kind: "color"           // "color" | "linear" | "radial"
    property color fillColor: "transparent" // opaque; alpha rides on the Shape's opacity
    property real r0: 0                     // topLeft radius
    property real r1: 0                     // topRight radius
    property real r2: 0                     // bottomRight radius
    property real r3: 0                     // bottomLeft radius
    property real angle: 180                // CSS linear angle (degrees; 0=up, CW)
    property real cx: 0.5                   // radial centre X (fraction of width)
    property real cy: 0.5                   // radial centre Y (fraction of height)
    property var spec: ({})                 // raw spec map — rebuildStops reads .stops

    // --- pure geometry (reactive to width/height) ------------------------------------
    // Linear gradient line: 0deg points up, increasing clockwise (CSS convention).
    readonly property real _angleRad: (layer.angle * Math.PI) / 180
    readonly property real _dx: Math.sin(layer._angleRad)
    readonly property real _dy: -Math.cos(layer._angleRad)
    readonly property real _len: Math.abs(width * layer._dx) + Math.abs(height * layer._dy)
    // Radial gradient centre in pixels.
    readonly property real cxp: layer.width * layer.cx
    readonly property real cyp: layer.height * layer.cy

    ShapePath {
        strokeWidth: 0
        strokeColor: "transparent"
        fillColor: (layer.kind === "color") ? layer.fillColor : "transparent"
        fillGradient: layer.kind === "linear" ? lin : (layer.kind === "radial" ? rad : null)

        startX: layer.r0; startY: 0
        PathLine { x: Math.max(layer.r0, layer.width - layer.r1); y: 0 }
        PathArc { x: layer.width; y: layer.r1; radiusX: layer.r1; radiusY: layer.r1; direction: PathArc.Clockwise }
        PathLine { x: layer.width; y: Math.max(layer.r1, layer.height - layer.r2) }
        PathArc { x: layer.width - layer.r2; y: layer.height; radiusX: layer.r2; radiusY: layer.r2; direction: PathArc.Clockwise }
        PathLine { x: layer.r3; y: layer.height }
        PathArc { x: 0; y: layer.height - layer.r3; radiusX: layer.r3; radiusY: layer.r3; direction: PathArc.Clockwise }
        PathLine { x: 0; y: layer.r0 }
        PathArc { x: layer.r0; y: 0; radiusX: layer.r0; radiusY: layer.r0; direction: PathArc.Clockwise }
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
        centerX: layer.cxp
        centerY: layer.cyp
        focalX: layer.cxp
        focalY: layer.cyp
        // CSS default size = farthest-corner: distance from centre to the furthest box corner.
        centerRadius: Math.sqrt(Math.pow(Math.max(layer.cxp, layer.width - layer.cxp), 2)
                              + Math.pow(Math.max(layer.cyp, layer.height - layer.cyp), 2))
    }

    // Stops are built imperatively (opaque — the layer's alpha is carried on `opacity`).
    Component { id: stopComponent; GradientStop {} }
    function rebuildStops() {
        var k = layer.kind
        if (k !== "linear" && k !== "radial")
            return
        var g = k === "linear" ? lin : rad
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
)QML";

CssFillLayer::CssFillLayer(QQuickItem *parent)
    : QQuickItem(parent)
{
}

void CssFillLayer::setSpec(const QVariantMap &v)
{
    if (m_spec == v)
        return;
    m_spec = v;
    emit specChanged();
    recompute();
}

void CssFillLayer::setRadii(const QVariantList &v)
{
    if (m_radii == v)
        return;
    m_radii = v;
    emit radiiChanged();
    recompute();
}

static qreal radiiAt(const QVariantList &radii, int i)
{
    return (i >= 0 && i < radii.size()) ? radii.at(i).toReal() : 0.0;
}

void CssFillLayer::recompute()
{
    if (!m_shape)
        return;

    const QString kind = m_spec.contains(QStringLiteral("type"))
        ? m_spec.value(QStringLiteral("type")).toString()
        : QStringLiteral("color");

    // peakAlpha: matches the QML `readonly property real peakAlpha` binding exactly.
    // For "color": the solid colour's alpha (or 0 when spec.color is absent/invalid).
    // For gradient: max alpha across all stops (or 1 when stops list is absent).
    qreal peakAlpha;
    if (kind == QLatin1String("color")) {
        const QColor c = m_spec.value(QStringLiteral("color")).value<QColor>();
        peakAlpha = c.isValid() ? c.alphaF() : 0.0;
    } else {
        const QVariantList stops = m_spec.value(QStringLiteral("stops")).toList();
        if (stops.isEmpty()) {
            peakAlpha = 1.0;
        } else {
            peakAlpha = 0.0;
            for (const QVariant &st : stops)
                peakAlpha = std::max<qreal>(
                    peakAlpha,
                    st.toMap().value(QStringLiteral("color")).value<QColor>().alphaF());
        }
    }

    // Opaque fill colour (alpha is carried on the Shape's opacity via peakAlpha).
    QColor fillColor(Qt::transparent);
    if (kind == QLatin1String("color")) {
        const QColor c = m_spec.value(QStringLiteral("color")).value<QColor>();
        if (c.isValid()) {
            fillColor = c;
            fillColor.setAlphaF(1.0);
        }
    }

    // CSS linear-gradient angle (degrees; 0=up, CW). QML default: 180 = top→bottom.
    const qreal angle = m_spec.contains(QStringLiteral("angle"))
        ? m_spec.value(QStringLiteral("angle")).toReal() : 180.0;

    // Radial gradient centre fractions (CSS default 0.5, 0.5 = centre of the box).
    const qreal cx = m_spec.contains(QStringLiteral("cx"))
        ? m_spec.value(QStringLiteral("cx")).toReal() : 0.5;
    const qreal cy = m_spec.contains(QStringLiteral("cy"))
        ? m_spec.value(QStringLiteral("cy")).toReal() : 0.5;

    // Push all inputs onto the shell. `kind` is set before `spec` so that when onSpecChanged
    // fires (from the setProperty("spec") call below) rebuildStops() reads the correct kind.
    m_shape->setProperty("kind", kind);
    m_shape->setProperty("fillColor", fillColor);
    m_shape->setProperty("r0", radiiAt(m_radii, 0));
    m_shape->setProperty("r1", radiiAt(m_radii, 1));
    m_shape->setProperty("r2", radiiAt(m_radii, 2));
    m_shape->setProperty("r3", radiiAt(m_radii, 3));
    m_shape->setProperty("angle", angle);
    m_shape->setProperty("cx", cx);
    m_shape->setProperty("cy", cy);
    // Setting spec last triggers onSpecChanged → rebuildStops() in the shell.
    m_shape->setProperty("spec", m_spec);
    // The Shape's opacity carries the fill alpha (QML: `opacity: layer.peakAlpha`).
    m_shape->setProperty("opacity", peakAlpha);
}

void CssFillLayer::layoutChild()
{
    // C++ equivalent of `anchors.fill: parent` on the inner Shape.
    if (!m_shape)
        return;
    m_shape->setX(0);
    m_shape->setY(0);
    m_shape->setWidth(width());
    m_shape->setHeight(height());
}

void CssFillLayer::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlEngine *eng = qmlEngine(this)) {
        QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssfilllayer-01c7e117"),
            kFillLayerShell);
        if (QObject *o = comp->create(qmlContext(this))) {
            if (QQuickItem *shape = qobject_cast<QQuickItem *>(o)) {
                shape->setParentItem(this);
                m_shape = shape;
            } else {
                o->deleteLater();
            }
        } else {
            qWarning("CssFillLayer: failed to compose Shape shell: %s",
                     qPrintable(comp->errorString()));
        }
    }

    layoutChild();
    recompute();
}

void CssFillLayer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChild();
}
