#include "qmlcss/componentcache.h"
#include "qmlcss/cssrect.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"
#include "qmlcss/valueparser.h"

#include <QEasingCurve>
#include <QElapsedTimer>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QVariantAnimation>
#include <QQmlEngine>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QRegularExpression>
#include <QVarLengthArray>
#include <QUrl>

#include <algorithm>

namespace {

// SQ_MOUNT_STATS=1: aggregate per-phase mount timing (style resolve vs shell composition),
// dumped at exit — the "before-mount / on-mount" breakdown.
struct MountStats {
    qint64 resolveNs = 0;
    qint64 shellNs = 0;
    qint64 restNs = 0;
    int count = 0;
    bool enabled = qEnvironmentVariableIntValue("SQ_MOUNT_STATS") != 0;
    ~MountStats()
    {
        if (enabled && count > 0)
            fprintf(stderr,
                    "[mount-stats] CssRect x%d | before-mount(resolve): %.1fms | shell: %.1fms | rest: %.1fms\n",
                    count, resolveNs / 1e6, shellNs / 1e6, restNs / 1e6);
    }
};
MountStats g_mountStats;


// Mirror of csstheme.cpp's identity coercion, used only for the hasCssIdentity check.
QStringList toStringList(const QVariant &v)
{
    if (!v.isValid())
        return {};
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return toStringList(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QStringList)
        return v.toStringList();
    if (v.metaType().id() == QMetaType::QVariantList) {
        QStringList out;
        const QVariantList list = v.toList();
        for (const QVariant &e : list) {
            const QString s = e.toString().trimmed();
            if (!s.isEmpty())
                out << s;
        }
        return out;
    }
    return v.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool isCssUrl(const QString &value)
{
    return value.trimmed().toLower().startsWith(QLatin1String("url("));
}

// JS parseFloat: leading numeric prefix ("2px" -> 2), NaN-ish -> fallback.
qreal parseLeadingFloat(const QString &value, qreal fallback)
{
    const QString t = value.trimmed();
    int i = 0;
    const int n = t.size();
    if (i < n && (t[i] == QLatin1Char('+') || t[i] == QLatin1Char('-')))
        ++i;
    int digits = 0;
    while (i < n && (t[i].isDigit() || t[i] == QLatin1Char('.'))) {
        if (t[i].isDigit())
            ++digits;
        ++i;
    }
    if (!digits)
        return fallback;
    bool ok = false;
    const qreal r = t.left(i).toDouble(&ok);
    return ok ? r : fallback;
}

QColor opaque(const QColor &c)
{
    QColor o = c;
    o.setAlphaF(1.0);
    return o;
}

// The composed render subtree — a REAL QtQuick vector tree driven entirely by the properties
// C++ pushes onto its root (recompute()). Rectangle is avoided (owner's rule); every filled
// bar is a Shape + PathRectangle. Pure geometry (radius clamping, gradient line, bevel edges)
// is JS on the root's own width/height, exactly like CssRect.qml.
const char *kRenderShell = R"QML(
import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import qmlcss

Item {
    id: r

    // --- inputs pushed by C++ — ONE map, written in a single setProperty per style apply.
    // Per-property pushes re-ran every dependent JS binding once PER PROPERTY (35 writes ->
    // hundreds of evaluations per element during page creation); off one map, each derived
    // binding re-evaluates exactly once per apply.
    property var cssIn: ({})
    readonly property string radiusStr: cssIn.radiusStr !== undefined ? cssIn.radiusStr : ""
    readonly property real radiusFallback: cssIn.radiusFallback !== undefined ? cssIn.radiusFallback : 0
    readonly property color solid: cssIn.solid !== undefined ? cssIn.solid : "transparent"
    readonly property real fillAlpha: cssIn.fillAlpha !== undefined ? cssIn.fillAlpha : 1
    readonly property bool hasGradient: cssIn.hasGradient !== undefined ? cssIn.hasGradient : false
    readonly property var gradient: cssIn.gradient !== undefined ? cssIn.gradient : ({})
    readonly property var orderedLayers: cssIn.orderedLayers !== undefined ? cssIn.orderedLayers : []
    readonly property bool borderVisible: cssIn.borderVisible !== undefined ? cssIn.borderVisible : false
    readonly property color borderColorOpaque: cssIn.borderColorOpaque !== undefined ? cssIn.borderColorOpaque : "transparent"
    readonly property real borderWidth: cssIn.borderWidth !== undefined ? cssIn.borderWidth : 0
    readonly property real borderAlpha: cssIn.borderAlpha !== undefined ? cssIn.borderAlpha : 1
    readonly property bool hasSideBorder: cssIn.hasSideBorder !== undefined ? cssIn.hasSideBorder : false
    readonly property var topB: cssIn.topB !== undefined ? cssIn.topB : ({})
    readonly property var rightB: cssIn.rightB !== undefined ? cssIn.rightB : ({})
    readonly property var bottomB: cssIn.bottomB !== undefined ? cssIn.bottomB : ({})
    readonly property var leftB: cssIn.leftB !== undefined ? cssIn.leftB : ({})
    readonly property bool hasOutsetShadow: cssIn.hasOutsetShadow !== undefined ? cssIn.hasOutsetShadow : false
    readonly property color shadowFillColor: cssIn.shadowFillColor !== undefined ? cssIn.shadowFillColor : "#ffffff"
    readonly property real shadowSrcOpacity: cssIn.shadowSrcOpacity !== undefined ? cssIn.shadowSrcOpacity : 1
    readonly property color shadowColorOpaque: cssIn.shadowColorOpaque !== undefined ? cssIn.shadowColorOpaque : "transparent"
    readonly property real shadowOpacity: cssIn.shadowOpacity !== undefined ? cssIn.shadowOpacity : 0
    readonly property real shadowX: cssIn.shadowX !== undefined ? cssIn.shadowX : 0
    readonly property real shadowY: cssIn.shadowY !== undefined ? cssIn.shadowY : 0
    readonly property real shadowBlur: cssIn.shadowBlur !== undefined ? cssIn.shadowBlur : 0
    readonly property bool insetBevel: cssIn.insetBevel !== undefined ? cssIn.insetBevel : false
    readonly property color insetDark: cssIn.insetDark !== undefined ? cssIn.insetDark : "transparent"
    readonly property color insetLight: cssIn.insetLight !== undefined ? cssIn.insetLight : Qt.rgba(1, 1, 1, 0.30)
    readonly property bool insetEdge: cssIn.insetEdge !== undefined ? cssIn.insetEdge : false
    readonly property real insetEdgeX: cssIn.insetEdgeX !== undefined ? cssIn.insetEdgeX : 0
    readonly property real insetEdgeY: cssIn.insetEdgeY !== undefined ? cssIn.insetEdgeY : 0
    readonly property color insetEdgeColor: cssIn.insetEdgeColor !== undefined ? cssIn.insetEdgeColor : "transparent"
    readonly property int transitionMs: cssIn.transitionMs !== undefined ? cssIn.transitionMs : 0
    readonly property int transitionEasing: cssIn.transitionEasing !== undefined ? cssIn.transitionEasing : Easing.InOutQuad

    // --- pure geometry (no cssTheme) ---------------------------------------------------
    function parseCssLength(value, fallback) {
        if (value && String(value).trim().indexOf("%") > 0) {
            var pct = parseFloat(value)
            return isNaN(pct) ? fallback : Math.min(r.width, r.height) * pct / 100.0
        }
        var nnn = parseFloat(value)
        return isNaN(nnn) ? fallback : nnn
    }
    function parseBorderRadius(value) {
        if (!value || String(value).trim().length === 0)
            return [r.radiusFallback, r.radiusFallback, r.radiusFallback, r.radiusFallback]
        var s = String(value).split("/")[0].trim()
        var parts = s.split(/\s+/)
        var values = []
        for (var i = 0; i < parts.length && i < 4; ++i)
            values.push(r.parseCssLength(parts[i], r.radiusFallback))
        if (values.length === 0)
            return [r.radiusFallback, r.radiusFallback, r.radiusFallback, r.radiusFallback]
        if (values.length === 1)
            return [values[0], values[0], values[0], values[0]]
        if (values.length === 2)
            return [values[0], values[1], values[0], values[1]]
        if (values.length === 3)
            return [values[0], values[1], values[2], values[1]]
        return [values[0], values[1], values[2], values[3]]
    }
    readonly property var borderRadii: r.parseBorderRadius(r.radiusStr)
    // Pre-clamped corner radii as PROPERTIES: the path elements below used to call
    // clampedRadius() from every x/y/radius binding — 7000+ JS function calls on one page
    // mount (the single hottest line in the click profile). A property read is cheap and
    // re-evaluates only when radii or size actually change.
    readonly property real halfMin: Math.min(r.width / 2, r.height / 2)
    readonly property real cr0: Math.max(0, Math.min(r.borderRadii[0], r.halfMin))
    readonly property real cr1: Math.max(0, Math.min(r.borderRadii[1], r.halfMin))
    readonly property real cr2: Math.max(0, Math.min(r.borderRadii[2], r.halfMin))
    readonly property real cr3: Math.max(0, Math.min(r.borderRadii[3], r.halfMin))
    function clampedRadius(index) {
        return index === 0 ? r.cr0 : index === 1 ? r.cr1 : index === 2 ? r.cr2 : r.cr3
    }
    readonly property bool partiallyRounded: r.borderRadii[0] > 0 || r.borderRadii[1] > 0
        || r.borderRadii[2] > 0 || r.borderRadii[3] > 0
    function bevelEdge(a, b) {
        return !(r.partiallyRounded && r.borderRadii[a] <= 0 && r.borderRadii[b] <= 0)
    }

    // gradient line: 0deg points up, increasing clockwise.
    readonly property real angleRad: ((r.hasGradient && r.gradient.angle !== undefined ? r.gradient.angle : 180) * Math.PI) / 180
    readonly property real dirX: Math.sin(r.angleRad)
    readonly property real dirY: -Math.cos(r.angleRad)
    readonly property real lineLen: Math.abs(r.width * r.dirX) + Math.abs(r.height * r.dirY)

    // A filled axis-aligned bar (a Rectangle replacement) for per-side borders and bevels.
    component Bar : Shape {
        id: bar
        property color barColor: "transparent"
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: bar.barColor
            PathRectangle { x: 0; y: 0; width: bar.width; height: bar.height }
        }
    }

    // --- outset box-shadow (a separate source behind the fill, blurred by MultiEffect) ---
    // Composed LAZILY: a MultiEffect drags in ShaderEffectSources + blur items even when
    // invisible — 150+ dormant effect stacks made a page cost thousands of nodes.
    Loader {
        anchors.fill: parent
        active: r.hasOutsetShadow
        sourceComponent: Item {
        anchors.fill: parent

        Shape {
            id: shadowSource
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: r.shadowSrcOpacity
            ShapePath {
                strokeColor: "transparent"
                strokeWidth: 0
                fillColor: r.shadowFillColor
                startX: r.cr0; startY: 0
                PathLine { x: Math.max(r.cr0, r.width - r.cr1); y: 0 }
                PathArc { x: r.width; y: r.cr1; radiusX: r.cr1; radiusY: r.cr1; direction: PathArc.Clockwise }
                PathLine { x: r.width; y: Math.max(r.cr1, r.height - r.cr2) }
                PathArc { x: r.width - r.cr2; y: r.height; radiusX: r.cr2; radiusY: r.cr2; direction: PathArc.Clockwise }
                PathLine { x: r.cr3; y: r.height }
                PathArc { x: 0; y: r.height - r.cr3; radiusX: r.cr3; radiusY: r.cr3; direction: PathArc.Clockwise }
                PathLine { x: 0; y: r.cr0 }
                PathArc { x: r.cr0; y: 0; radiusX: r.cr0; radiusY: r.cr0; direction: PathArc.Clockwise }
            }
        }
        MultiEffect {
            anchors.fill: shadowSource
            source: shadowSource
            shadowEnabled: true
            shadowColor: r.shadowColorOpaque
            shadowOpacity: r.shadowOpacity
            shadowHorizontalOffset: r.shadowX
            shadowVerticalOffset: r.shadowY
            shadowBlur: r.shadowBlur
            autoPaddingEnabled: true
        }
        }
    }

    // --- the fill (solid colour or linear gradient). Alpha rides on opacity. Lazy: most
    // boxes are layout-only (transparent) and skip the whole path/gradient machinery. -----
    readonly property bool hasFill: (r.solid.a > 0) || r.hasGradient
    Loader {
        anchors.fill: parent
        active: r.hasFill
        sourceComponent: Shape {
        id: fill
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        opacity: r.fillAlpha
        Behavior on opacity {
            enabled: r.transitionMs > 0
            NumberAnimation { duration: r.transitionMs; easing.type: r.transitionEasing }
        }
        ShapePath {
            strokeColor: "transparent"
            strokeWidth: 0
            fillColor: r.hasGradient ? "transparent" : r.solid
            fillGradient: r.hasGradient ? linearGradient : null
            Behavior on fillColor {
                enabled: r.transitionMs > 0
                ColorAnimation { duration: r.transitionMs; easing.type: r.transitionEasing }
            }
            startX: r.cr0; startY: 0
            PathLine { x: Math.max(r.cr0, r.width - r.cr1); y: 0 }
            PathArc { x: r.width; y: r.cr1; radiusX: r.cr1; radiusY: r.cr1; direction: PathArc.Clockwise }
            PathLine { x: r.width; y: Math.max(r.cr1, r.height - r.cr2) }
            PathArc { x: r.width - r.cr2; y: r.height; radiusX: r.cr2; radiusY: r.cr2; direction: PathArc.Clockwise }
            PathLine { x: r.cr3; y: r.height }
            PathArc { x: 0; y: r.height - r.cr3; radiusX: r.cr3; radiusY: r.cr3; direction: PathArc.Clockwise }
            PathLine { x: 0; y: r.cr0 }
            PathArc { x: r.cr0; y: 0; radiusX: r.cr0; radiusY: r.cr0; direction: PathArc.Clockwise }
        }
        LinearGradient {
            id: linearGradient
            x1: r.width / 2 - r.dirX * r.lineLen / 2
            y1: r.height / 2 - r.dirY * r.lineLen / 2
            x2: r.width / 2 + r.dirX * r.lineLen / 2
            y2: r.height / 2 + r.dirY * r.lineLen / 2
        }
        // Opaque gradient stops; the gradient's alpha is carried on this Shape's opacity.
        Component { id: stopComponent; GradientStop {} }
        function rebuildStops() {
            var built = []
            var g = r.gradient
            var list = (g && g.stops !== undefined) ? g.stops : []
            for (var i = 0; i < list.length; ++i) {
                var c = list[i].color
                built.push(stopComponent.createObject(linearGradient,
                    { position: list[i].position, color: Qt.rgba(c.r, c.g, c.b, 1.0) }))
            }
            linearGradient.stops = built
        }
        property var gradTrack: r.hasGradient ? r.gradient : null
        onGradTrackChanged: rebuildStops()
        Component.onCompleted: rebuildStops()
        }
    }

    // --- stacked background layers (radial / multi-layer) the single fill can't express --
    Repeater {
        model: r.orderedLayers
        CssFillLayer {
            anchors.fill: parent
            spec: modelData
            radii: [r.cr0, r.cr1, r.cr2, r.cr3]
        }
    }

    // --- border as its own Shape (independent alpha); lazy — most boxes have no border ---
    Loader {
        anchors.fill: parent
        active: r.borderVisible
        sourceComponent: Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        opacity: r.borderAlpha
        ShapePath {
            strokeColor: r.borderColorOpaque
            strokeWidth: r.borderWidth
            fillColor: "transparent"
            startX: r.cr0; startY: 0
            PathLine { x: Math.max(r.cr0, r.width - r.cr1); y: 0 }
            PathArc { x: r.width; y: r.cr1; radiusX: r.cr1; radiusY: r.cr1; direction: PathArc.Clockwise }
            PathLine { x: r.width; y: Math.max(r.cr1, r.height - r.cr2) }
            PathArc { x: r.width - r.cr2; y: r.height; radiusX: r.cr2; radiusY: r.cr2; direction: PathArc.Clockwise }
            PathLine { x: r.cr3; y: r.height }
            PathArc { x: 0; y: r.height - r.cr3; radiusX: r.cr3; radiusY: r.cr3; direction: PathArc.Clockwise }
            PathLine { x: 0; y: r.cr0 }
            PathArc { x: r.cr0; y: 0; radiusX: r.cr0; radiusY: r.cr0; direction: PathArc.Clockwise }
        }
        }
    }

    // --- per-side CSS borders (border-top, etc.); lazy ----------------------------------
    Loader {
        anchors.fill: parent
        active: r.hasSideBorder
        sourceComponent: Item {
        anchors.fill: parent
        Bar {
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: r.topB.visible ? Math.max(1, r.topB.width) : 0
            barColor: r.topB.color ? r.topB.color : "transparent"
            visible: !!r.topB.visible
        }
        Bar {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: r.bottomB.visible ? Math.max(1, r.bottomB.width) : 0
            barColor: r.bottomB.color ? r.bottomB.color : "transparent"
            visible: !!r.bottomB.visible
        }
        Bar {
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: r.leftB.visible ? Math.max(1, r.leftB.width) : 0
            barColor: r.leftB.color ? r.leftB.color : "transparent"
            visible: !!r.leftB.visible
        }
        Bar {
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: r.rightB.visible ? Math.max(1, r.rightB.width) : 0
            barColor: r.rightB.color ? r.rightB.color : "transparent"
            visible: !!r.rightB.visible
        }
        }
    }

    // --- sunken bevel for two inset box-shadows; lazy -----------------------------------
    Loader {
        anchors.fill: parent
        active: r.insetBevel
        sourceComponent: Item {
        anchors.fill: parent
        Bar { // top — shadow
            visible: r.bevelEdge(0, 1)
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.leftMargin: r.cr0; anchors.rightMargin: r.cr1
            height: 1; barColor: r.insetDark
        }
        Bar { // left — shadow
            visible: r.bevelEdge(0, 3)
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: r.cr0; anchors.bottomMargin: r.cr3
            width: 1; barColor: r.insetDark
        }
        Bar { // bottom — highlight
            visible: r.bevelEdge(3, 2)
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: r.cr3; anchors.rightMargin: r.cr2
            height: 1; barColor: r.insetLight
        }
        Bar { // right — highlight
            visible: r.bevelEdge(1, 2)
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: r.cr1; anchors.bottomMargin: r.cr2
            width: 1; barColor: r.insetLight
        }
        }
    }

    // --- a single inset box-shadow = a directional inner edge; lazy ---------------------
    Loader {
        anchors.fill: parent
        active: r.insetEdge
        sourceComponent: Item {
        anchors.fill: parent
        Bar { // top/bottom band
            visible: r.insetEdge && r.insetEdgeY !== 0 && Math.abs(r.insetEdgeY) >= Math.abs(r.insetEdgeX)
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: r.insetEdgeY > 0 ? parent.top : undefined
            anchors.bottom: r.insetEdgeY < 0 ? parent.bottom : undefined
            height: Math.max(1, Math.abs(r.insetEdgeY))
            barColor: r.insetEdgeColor
        }
        Bar { // left/right band
            visible: r.insetEdge && r.insetEdgeX !== 0 && Math.abs(r.insetEdgeX) > Math.abs(r.insetEdgeY)
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.left: r.insetEdgeX > 0 ? parent.left : undefined
            anchors.right: r.insetEdgeX < 0 ? parent.right : undefined
            width: Math.max(1, Math.abs(r.insetEdgeX))
            barColor: r.insetEdgeColor
        }
        }
    }

}
)QML";

} // namespace

CssRect::CssRect(QQuickItem *parent)
    : QQuickItem(parent)
{
    // The content holder must exist BEFORE the QML engine appends declared children to `content`
    // (which happens during parsing, before componentComplete). It is a plain Item (no engine
    // needed), sized to us, hosting the layout participants — exactly the QML `contentHolder`.
    m_contentHolder = new QQuickItem(this);
    connect(m_contentHolder, &QQuickItem::childrenChanged, this, [this]() { requestRelayout(); });
}

bool CssRect::hasCssIdentity() const
{
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    return !toStringList(m_cssClass).isEmpty();
}

void CssRect::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssRect::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssRect::setCssClass(const QVariant &v)
{
    // Value-compare: QML re-binds hand a FRESH array each evaluation; an equal
    // list must not trigger a restyle (it double-applied every element on mount).
    if (m_cssClass == v || toStringList(m_cssClass) == toStringList(v))
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssRect::setCssState(const QVariant &v)
{
    // Value-compare: QML re-binds hand a FRESH array each evaluation; an equal
    // list must not trigger a restyle (it double-applied every element on mount).
    if (m_cssState == v || toStringList(m_cssState) == toStringList(v))
        return;
    m_cssState = v;
    emit cssStateChanged();
    maybeLoadCss();
}

void CssRect::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssRect::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
}

void CssRect::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();

    // display:none NEVER writes `visible` (any imperative write severs author bindings —
    // the <Show> guard — and a later restore resurrects hidden delegates). The layout skips
    // display:none by style; painting dies via opacity 0; input via enabled false.
    const bool displayNone = m_style.value(QStringLiteral("display")).toString() == QLatin1String("none");

    // Parse the transition spec FIRST: the opacity write below and the layout engine's
    // width/height writes (via animateGeometry) both key off it.
    m_transMs = 0;
    m_transProp.clear();
    if (m_theme && m_style.contains(QStringLiteral("transition"))) {
        const QVariantMap spec = m_theme->parseTransition(m_style.value(QStringLiteral("transition")).toString());
        m_transMs = spec.value(QStringLiteral("duration")).toInt();
        m_transEasing = spec.value(QStringLiteral("easing"),
                                   static_cast<int>(QEasingCurve::InOutQuad)).toInt();
        m_transProp = spec.value(QStringLiteral("property")).toString();
    }

    // visibility:hidden -> painted invisible (opacity 0) and inert (enabled false), but STILL in flow.
    const bool hidden = displayNone
        || m_style.value(QStringLiteral("visibility")).toString() == QLatin1String("hidden");
    const qreal targetOpacity = hidden ? 0.0
        : (m_style.contains(QStringLiteral("opacity")) ? m_style.value(QStringLiteral("opacity")).toReal() : 1.0);
    // A covering `transition: opacity` fades instead of snapping (the carousel crossfade).
    if (!hidden && isComponentComplete() && transitionCovers(QLatin1String("opacity"))
        && !qFuzzyCompare(opacity(), targetOpacity)) {
        if (!m_opacityAnim) {
            m_opacityAnim = new QVariantAnimation(this);
            connect(m_opacityAnim, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant &v) { setOpacity(v.toReal()); });
        }
        m_opacityAnim->stop();
        m_opacityAnim->setStartValue(opacity());
        m_opacityAnim->setEndValue(targetOpacity);
        m_opacityAnim->setDuration(m_transMs);
        m_opacityAnim->setEasingCurve(static_cast<QEasingCurve::Type>(m_transEasing));
        m_opacityAnim->start();
    } else {
        if (m_opacityAnim)
            m_opacityAnim->stop();
        setOpacity(targetOpacity);
    }
    setEnabled(!hidden);
    setZ(m_style.contains(QStringLiteral("z-index")) ? m_style.value(QStringLiteral("z-index")).toReal() : 0.0);
    const QString overflow = m_style.value(QStringLiteral("overflow")).toString();
    QString overflowY = m_style.value(QStringLiteral("overflow-y")).toString();
    if (overflowY.isEmpty())
        overflowY = overflow;
    // overflow(-y): auto/scroll -> the content area becomes a REAL Flickable (QML's native
    // scrolling). Composed once, lazily; it clips itself, so the box's own clip stays off.
    if (overflowY == QLatin1String("auto") || overflowY == QLatin1String("scroll"))
        ensureScrollable();
    setClip(!m_flickable && (overflow == QLatin1String("hidden") || overflow == QLatin1String("clip")));

    recompute();
    // Pre-mount applies must NOT trigger layout: componentComplete finishes with ONE
    // requestRelayout for the whole element (the in-construction storm was O(N) flushes).
    if (isComponentComplete()) {
        requestRelayout();
        if (m_layout)
            m_layout->notifyParentLayout(this);
    }
    emit inheritedChanged();
}

void CssRect::setRadius(qreal v)
{
    if (qFuzzyCompare(m_radius, v))
        return;
    m_radius = v;
    emit radiusChanged();
    recompute();
}

void CssRect::setDefaultColor(const QColor &v)
{
    if (m_defaultColor == v)
        return;
    m_defaultColor = v;
    emit defaultColorChanged();
    recompute();
}

void CssRect::setDefaultBorderColor(const QColor &v)
{
    if (m_defaultBorderColor == v)
        return;
    m_defaultBorderColor = v;
    emit defaultBorderColorChanged();
    recompute();
}

void CssRect::setDefaultBorderWidth(qreal v)
{
    if (qFuzzyCompare(m_defaultBorderWidth, v))
        return;
    m_defaultBorderWidth = v;
    emit defaultBorderWidthChanged();
    recompute();
}

static QObject *cssInheritingAncestor(const QQuickItem *self)
{
    QQuickItem *holder = self->parentItem();
    QQuickItem *box = holder ? holder->parentItem() : nullptr;
    if (box && box->property("inheritedColor").isValid())
        return box;
    return nullptr;
}

CssRect *CssRect::cssParent() const
{
    return qobject_cast<CssRect *>(cssInheritingAncestor(this));
}

#define CSSRECT_INHERIT(Getter, Key)                                                                \
    QString CssRect::Getter() const                                                                \
    {                                                                                              \
        const QString own = m_style.value(QStringLiteral(Key)).toString();                        \
        if (!own.isEmpty())                                                                        \
            return own;                                                                            \
        QObject *anc = cssInheritingAncestor(this);                                                \
        return anc ? anc->property(#Getter).toString() : QString();                                \
    }

CSSRECT_INHERIT(inheritedColor, "color")
CSSRECT_INHERIT(inheritedFontFamily, "font-family")
CSSRECT_INHERIT(inheritedFontSize, "font-size")
CSSRECT_INHERIT(inheritedFontWeight, "font-weight")
CSSRECT_INHERIT(inheritedLineHeight, "line-height")
CSSRECT_INHERIT(inheritedLetterSpacing, "letter-spacing")
CSSRECT_INHERIT(inheritedTextTransform, "text-transform")
CSSRECT_INHERIT(inheritedTextAlign, "text-align")

#undef CSSRECT_INHERIT

void CssRect::setAnimRotate(qreal v)
{
    if (qFuzzyCompare(m_animRotate, v))
        return;
    m_animRotate = v;
    emit animChanged();
    applyTransform();
}

void CssRect::setAnimScale(qreal v)
{
    if (qFuzzyCompare(m_animScale, v))
        return;
    m_animScale = v;
    emit animChanged();
    applyTransform();
}

void CssRect::setAnimTx(qreal v)
{
    if (qFuzzyCompare(m_animTx, v))
        return;
    m_animTx = v;
    emit animChanged();
    applyTransform();
}

void CssRect::setAnimTy(qreal v)
{
    if (qFuzzyCompare(m_animTy, v))
        return;
    m_animTy = v;
    emit animChanged();
    applyTransform();
}

void CssRect::setAnimTick(qreal v)
{
    m_animTick = v;
    emit animTickChanged();
    // Per-frame path: interpolate the PRE-PARSED stops and write the anim fields directly.
    // (The QML-equivalent went through cssLayout.applyAnim with QVariantMaps — allocation
    // churn on every tick of every animated element.)
    const int n = m_animStopsFast.size();
    if (n < 2)
        return;
    const AnimStop *a = &m_animStopsFast[0];
    const AnimStop *b = &m_animStopsFast[n - 1];
    for (int i = 0; i < n - 1; ++i) {
        if (v >= m_animStopsFast[i].offset && v <= m_animStopsFast[i + 1].offset) {
            a = &m_animStopsFast[i];
            b = &m_animStopsFast[i + 1];
            break;
        }
    }
    const double span = b->offset - a->offset;
    const double t = span > 0 ? (v - a->offset) / span : 0;
    m_animRotate = a->rotate + (b->rotate - a->rotate) * t;
    m_animScale = a->scale + (b->scale - a->scale) * t;
    m_animTx = a->tx + (b->tx - a->tx) * t;
    m_animTy = a->ty + (b->ty - a->ty) * t;
    emit animChanged();
    applyTransform();
}

// --- content default property (forwards to the contentHolder's `data`, the QML alias) --------

QQmlListProperty<QObject> CssRect::content()
{
    return QQmlListProperty<QObject>(
        this, nullptr,
        [](QQmlListProperty<QObject> *p, QObject *o) {
            auto *self = static_cast<CssRect *>(p->object);
            if (!self->m_contentHolder || !o)
                return;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            if (ref.isValid())
                ref.append(o);
        },
        [](QQmlListProperty<QObject> *p) -> qsizetype {
            auto *self = static_cast<CssRect *>(p->object);
            if (!self->m_contentHolder)
                return 0;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            return ref.isValid() ? ref.count() : 0;
        },
        [](QQmlListProperty<QObject> *p, qsizetype i) -> QObject * {
            auto *self = static_cast<CssRect *>(p->object);
            if (!self->m_contentHolder)
                return nullptr;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            return ref.isValid() ? ref.at(i) : nullptr;
        },
        [](QQmlListProperty<QObject> *p) {
            auto *self = static_cast<CssRect *>(p->object);
            if (!self->m_contentHolder)
                return;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            if (ref.isValid())
                ref.clear();
        });
}

void CssRect::requestRelayout()
{
    if (m_layout && m_contentHolder)
        m_layout->requestLayout(this, m_contentHolder);
}

void CssRect::maybeLoadCss()
{
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

// Does the resolved style paint anything? Only then is the render shell worth composing.
// (Transforms, opacity, animation and layout all live on the item itself.)
static bool stylePaints(const QVariantMap &style)
{
    static const char *paintKeys[] = {
        "background", "background-color", "background-image",
        "border", "border-color", "border-width",
        "border-top", "border-right", "border-bottom", "border-left",
        "box-shadow",
    };
    for (const char *k : paintKeys) {
        if (!style.value(QLatin1String(k)).toString().isEmpty())
            return true;
    }
    return false;
}

// The Shape x Rectangle POLICY (owner's design): the full Shape shell is only composed when
// the style demands what Rectangle cannot do. Conservative by explicit list — anything not
// provably rectangle-safe takes the Shape path. Qt >= 6.7 Rectangle covers per-corner radii.
static bool needsShape(const QVariantMap &style)
{
    const auto str = [&](const char *k) { return style.value(QLatin1String(k)).toString(); };
    // BOTH background keys can be present (different cascade rules contribute each); the
    // shell paints the gradient when either carries one — the policy must mirror that.
    for (const char *k : { "background", "background-color" }) {
        const QString v = str(k);
        if (v.contains(QLatin1String("gradient(")) || v.contains(QLatin1String("url(")))
            return true;
    }
    if (!str("background-image").isEmpty())
        return true;
    if (!str("box-shadow").isEmpty())
        return true; // outset shadow sources a Shape; inset bevels are Shape bars
    for (const char *side : { "border-top", "border-right", "border-bottom", "border-left" }) {
        if (!str(side).isEmpty())
            return true; // per-side borders
    }
    if (str("border-radius").contains(QLatin1Char('%')))
        return true; // %-radii resolve against geometry in the shell
    return false;
}

// border-radius shorthand -> 4 px corners [tl, tr, br, bl] (no %, guaranteed by the policy).
static void parseRadiiPx(const QString &value, qreal fallback, qreal out[4])
{
    out[0] = out[1] = out[2] = out[3] = fallback;
    const QStringList parts = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QVarLengthArray<double, 4> v;
    for (const QString &part : parts) {
        double px = 0;
        if (CssValueParser::parseLengthPx(part, &px))
            v.append(px);
        if (v.size() == 4)
            break;
    }
    if (v.size() == 1) { out[0] = out[1] = out[2] = out[3] = v[0]; }
    else if (v.size() == 2) { out[0] = out[2] = v[0]; out[1] = out[3] = v[1]; }
    else if (v.size() == 3) { out[0] = v[0]; out[1] = out[3] = v[1]; out[2] = v[2]; }
    else if (v.size() == 4) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = v[3]; }
}

void CssRect::recompute()
{
    // Lean shell + Shape x Rectangle policy: nothing composes until the style paints, and a
    // paintable style takes the CHEAP QQuickRectangle path unless it demands the Shape shell
    // (gradients, per-side borders, shadows, %-radii). The verdict re-evaluates on EVERY
    // (re)apply — a hover/theme change swaps the composition.
    const bool paints = stylePaints(m_style);
    const bool shapePath = paints && needsShape(m_style);
    if (m_render && !shapePath) {
        m_render->deleteLater();
        m_render = nullptr;
    }
    if (m_fastRect && (shapePath || !paints)) {
        m_fastRect->deleteLater();
        m_fastRect = nullptr;
    }
    if (shapePath && !m_render)
        ensureRenderShell();
    if (!shapePath && paints) {
        ensureFastRect();
        pushFastRect();
    }
    if (!m_render) {
        applyStaticTransformAndAnim();
        return;
    }

    auto str = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };
    auto has = [&](const char *k) { return !m_style.value(QLatin1String(k)).toString().isEmpty(); };

    // --- background / gradient ---
    const QString backgroundValue = str("background");
    const QString bgValue = isCssUrl(backgroundValue) ? QString() : backgroundValue;
    const bool bgIsGradient = bgValue.contains(QLatin1String("gradient"));
    QVariantList bgLayers;
    if (m_theme && bgIsGradient)
        bgLayers = m_theme->parseGradientLayers(bgValue);
    const bool singleLinear = bgLayers.size() == 1
        && bgLayers.at(0).toMap().value(QStringLiteral("type")).toString() == QLatin1String("linear");
    const QVariantMap gradient = singleLinear ? bgLayers.at(0).toMap() : QVariantMap();
    const bool hasGradient = gradient.contains(QStringLiteral("stops"));
    const bool useLayeredFill = bgLayers.size() >= 1 && !singleLinear;
    QVariantList orderedLayers;
    if (useLayeredFill)
        for (int i = bgLayers.size() - 1; i >= 0; --i)
            orderedLayers.push_back(bgLayers.at(i));

    qreal gradientPeakAlpha = 1.0;
    if (hasGradient) {
        gradientPeakAlpha = 0.0;
        const QVariantList stops = gradient.value(QStringLiteral("stops")).toList();
        for (const QVariant &st : stops)
            gradientPeakAlpha = std::max<qreal>(gradientPeakAlpha,
                                                st.toMap().value(QStringLiteral("color")).value<QColor>().alphaF());
    }

    QColor solid;
    if (has("background-color") && m_theme)
        solid = m_theme->parseColor(str("background-color"));
    else if (!bgValue.isEmpty() && !bgIsGradient && m_theme)
        solid = m_theme->parseColor(bgValue);
    else
        solid = m_defaultColor;
    const qreal fillAlpha = hasGradient ? gradientPeakAlpha : solid.alphaF();

    // --- border ---
    QVariantMap borderShorthand;
    if (m_theme && has("border"))
        borderShorthand = m_theme->parseBorder(str("border"));
    QColor borderColor;
    if (has("border-color") && m_theme)
        borderColor = m_theme->parseColor(str("border-color"));
    else if (borderShorthand.contains(QStringLiteral("color")))
        borderColor = borderShorthand.value(QStringLiteral("color")).value<QColor>();
    else
        borderColor = m_defaultBorderColor;
    qreal borderWidth;
    if (has("border-width"))
        borderWidth = parseLeadingFloat(str("border-width"), m_defaultBorderWidth);
    else if (borderShorthand.contains(QStringLiteral("width")))
        borderWidth = borderShorthand.value(QStringLiteral("width")).toReal();
    else
        borderWidth = m_defaultBorderWidth;

    const bool hasSideBorder = m_theme ? m_theme->hasSideBorder(m_style) : false;
    const QVariantMap topB = m_theme ? m_theme->borderSide(m_style, QStringLiteral("top"), 0, borderColor) : QVariantMap();
    const QVariantMap rightB = m_theme ? m_theme->borderSide(m_style, QStringLiteral("right"), 0, borderColor) : QVariantMap();
    const QVariantMap bottomB = m_theme ? m_theme->borderSide(m_style, QStringLiteral("bottom"), 0, borderColor) : QVariantMap();
    const QVariantMap leftB = m_theme ? m_theme->borderSide(m_style, QStringLiteral("left"), 0, borderColor) : QVariantMap();
    const bool borderVisible = !hasSideBorder && borderWidth > 0 && borderColor.alphaF() > 0;

    // --- box-shadow ---
    QVariantList shadowList;
    if (m_theme && has("box-shadow"))
        shadowList = m_theme->parseBoxShadowList(str("box-shadow"));
    QVariantMap outset;
    for (const QVariant &sh : shadowList) {
        const QVariantMap m = sh.toMap();
        if (m.value(QStringLiteral("inset")).toBool() != true && m.contains(QStringLiteral("color"))) {
            outset = m;
            break;
        }
    }
    QVariantList insets;
    for (const QVariant &sh : shadowList) {
        const QVariantMap m = sh.toMap();
        if (m.value(QStringLiteral("inset")).toBool() == true)
            insets.push_back(m);
    }
    const bool hasOutsetShadow = outset.contains(QStringLiteral("color"));
    const bool insetBevel = insets.size() >= 2;
    const QVariantMap insetEdgeShadow = insets.size() == 1 ? insets.at(0).toMap() : QVariantMap();
    const bool insetEdge = !insetEdgeShadow.isEmpty();
    const QColor insetDark = insets.size() > 0 ? insets.at(0).toMap().value(QStringLiteral("color")).value<QColor>()
                                               : QColor(Qt::transparent);
    const QColor insetLight = insets.size() > 1 ? insets.at(1).toMap().value(QStringLiteral("color")).value<QColor>()
                                                : QColor::fromRgbF(1, 1, 1, 0.30);
    const QColor outColor = outset.value(QStringLiteral("color")).value<QColor>();

    // --- transition ---
    QVariantMap transition;
    if (m_theme && has("transition"))
        transition = m_theme->parseTransition(str("transition"));
    const int transitionMs = transition.contains(QStringLiteral("duration"))
        ? transition.value(QStringLiteral("duration")).toInt() : 0;
    const int transitionEasing = transition.contains(QStringLiteral("easing"))
        ? transition.value(QStringLiteral("easing")).toInt() : static_cast<int>(QEasingCurve::InOutQuad);

    // --- push everything onto the composed render root — ONE write (see cssIn) ---
    QVariantMap in;
    QQuickItem *g = m_render;
    in.insert(QStringLiteral("radiusStr"), QVariant::fromValue(str("border-radius")));
    in.insert(QStringLiteral("radiusFallback"), QVariant::fromValue(m_radius));
    in.insert(QStringLiteral("solid"), QVariant::fromValue(opaque(solid)));
    in.insert(QStringLiteral("fillAlpha"), QVariant::fromValue(fillAlpha));
    in.insert(QStringLiteral("hasGradient"), QVariant::fromValue(hasGradient));
    in.insert(QStringLiteral("gradient"), QVariant::fromValue(gradient));
    in.insert(QStringLiteral("orderedLayers"), QVariant::fromValue(orderedLayers));
    in.insert(QStringLiteral("borderVisible"), QVariant::fromValue(borderVisible));
    in.insert(QStringLiteral("borderColorOpaque"), QVariant::fromValue(opaque(borderColor)));
    in.insert(QStringLiteral("borderWidth"), QVariant::fromValue(borderWidth));
    in.insert(QStringLiteral("borderAlpha"), QVariant::fromValue(borderColor.alphaF()));
    in.insert(QStringLiteral("hasSideBorder"), QVariant::fromValue(hasSideBorder));
    in.insert(QStringLiteral("topB"), QVariant::fromValue(topB));
    in.insert(QStringLiteral("rightB"), QVariant::fromValue(rightB));
    in.insert(QStringLiteral("bottomB"), QVariant::fromValue(bottomB));
    in.insert(QStringLiteral("leftB"), QVariant::fromValue(leftB));
    in.insert(QStringLiteral("hasOutsetShadow"), QVariant::fromValue(hasOutsetShadow));
    in.insert(QStringLiteral("shadowFillColor"), QVariant::fromValue(hasGradient ? QColor(QStringLiteral("#ffffff")) : opaque(solid)));
    in.insert(QStringLiteral("shadowSrcOpacity"), QVariant::fromValue(hasGradient ? gradientPeakAlpha : solid.alphaF()));
    in.insert(QStringLiteral("shadowColorOpaque"), QVariant::fromValue(hasOutsetShadow ? opaque(outColor) : QColor(Qt::transparent)));
    in.insert(QStringLiteral("shadowOpacity"), QVariant::fromValue(hasOutsetShadow ? outColor.alphaF() : 0.0));
    in.insert(QStringLiteral("shadowX"), QVariant::fromValue(hasOutsetShadow ? outset.value(QStringLiteral("x")).toReal() : 0.0));
    in.insert(QStringLiteral("shadowY"), QVariant::fromValue(hasOutsetShadow ? outset.value(QStringLiteral("y")).toReal() : 0.0));
    in.insert(QStringLiteral("shadowBlur"), hasOutsetShadow
                  ? std::min<qreal>(1.0, outset.value(QStringLiteral("blur")).toReal() / 32.0) : 0.0);
    in.insert(QStringLiteral("insetBevel"), QVariant::fromValue(insetBevel));
    in.insert(QStringLiteral("insetDark"), QVariant::fromValue(insetDark));
    in.insert(QStringLiteral("insetLight"), QVariant::fromValue(insetLight));
    in.insert(QStringLiteral("insetEdge"), QVariant::fromValue(insetEdge));
    in.insert(QStringLiteral("insetEdgeX"), QVariant::fromValue(insetEdge ? insetEdgeShadow.value(QStringLiteral("x")).toReal() : 0.0));
    in.insert(QStringLiteral("insetEdgeY"), QVariant::fromValue(insetEdge ? insetEdgeShadow.value(QStringLiteral("y")).toReal() : 0.0));
    in.insert(QStringLiteral("insetEdgeColor"),
              QVariant::fromValue(insetEdge ? insetEdgeShadow.value(QStringLiteral("color")).value<QColor>()
                                            : QColor(Qt::transparent)));
    in.insert(QStringLiteral("transitionMs"), QVariant::fromValue(transitionMs));
    in.insert(QStringLiteral("transitionEasing"), QVariant::fromValue(transitionEasing));

    // Convert ONCE to a JS object: a QVariantMap-typed `var` re-converts on every property
    // read in the shell (36 derived bindings × map conversion each — the remaining mount cost).
    if (QQmlEngine *eng = qmlEngine(this))
        g->setProperty("cssIn", QVariant::fromValue(eng->toScriptValue(in)));
    else
        g->setProperty("cssIn", in);

    applyStaticTransformAndAnim();
}

// Item-level style effects that exist with or without the render shell: static transform,
// the @keyframes driver, and the final transform application.
void CssRect::applyStaticTransformAndAnim()
{
    auto str = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };
    auto has = [&](const char *k) { return !m_style.value(QLatin1String(k)).toString().isEmpty(); };

    // --- static transform (rotate/scale/translate) — applied to us so content transforms too ---
    QVariantMap tf;
    if (m_theme && has("transform"))
        tf = m_theme->parseTransform(str("transform"));
    m_staticRotate = tf.contains(QStringLiteral("rotate")) ? tf.value(QStringLiteral("rotate")).toReal() : 0.0;
    m_staticScale = tf.contains(QStringLiteral("scale")) ? tf.value(QStringLiteral("scale")).toReal() : 1.0;
    m_staticTx = tf.contains(QStringLiteral("translateX")) ? tf.value(QStringLiteral("translateX")).toReal() : 0.0;
    m_staticTy = tf.contains(QStringLiteral("translateY")) ? tf.value(QStringLiteral("translateY")).toReal() : 0.0;

    // --- @keyframes animation driver ---
    // QML equivalent:
    //   readonly property var _animation: (cssTheme && cssTheme.loaded && style["animation"])
    //       ? cssTheme.parseAnimation(style["animation"]) : ({})
    //   readonly property var _animFrames: _animation.name ? cssTheme.keyframes(_animation.name) : []
    //   readonly property var _animStops: cssLayout ? cssLayout.buildAnimStops(_animFrames) : []
    //   readonly property bool _animActive: _animStops.length >= 2
    //
    //   NumberAnimation on animTick {
    //       from: 0.0; to: 1.0
    //       duration: Math.max(1, _animation.duration !== undefined ? _animation.duration : 1000)
    //       loops: (_animation.iterations === undefined || _animation.iterations < 0)
    //                  ? Animation.Infinite : Math.max(1, _animation.iterations)
    //       easing.type: _animation.easing !== undefined ? _animation.easing : Easing.Linear
    //       running: root._animActive
    //   }
    //   onAnimTickChanged: if (cssLayout) cssLayout.applyAnim(root, _animStops, animTick)
    QVariantMap animCfg;
    if (m_theme && has("animation"))
        animCfg = m_theme->parseAnimation(str("animation"));

    QVariantList animFrames;
    if (m_theme && !animCfg.isEmpty() && !animCfg.value(QStringLiteral("name")).toString().isEmpty())
        animFrames = m_theme->keyframes(animCfg.value(QStringLiteral("name")).toString());

    QVariantList newStops;
    if (m_layout)
        newStops = m_layout->buildAnimStops(animFrames);
    m_animStops = newStops;
    m_animActive = m_animStops.size() >= 2;
    m_animStopsFast.clear();
    for (const QVariant &v : std::as_const(m_animStops)) {
        const QVariantMap m = v.toMap();
        m_animStopsFast.append({ m.value(QStringLiteral("offset")).toDouble(),
                                 m.value(QStringLiteral("rotate")).toDouble(),
                                 m.value(QStringLiteral("scale"), 1.0).toDouble(),
                                 m.value(QStringLiteral("tx")).toDouble(),
                                 m.value(QStringLiteral("ty")).toDouble() });
    }

    if (m_animActive)
        ensureAnim(); // lazy: only boxes with a live @keyframes pay for the animation object
    if (m_anim) {
        // Stop first so reconfiguration takes effect cleanly (matching QML reactive rebuild).
        m_anim->setProperty("running", false);

        // QML: duration: Math.max(1, _animation.duration !== undefined ? _animation.duration : 1000)
        // animCfg is {} when no animation key in style; parseAnimation always sets "duration".
        const int dur = animCfg.isEmpty() ? 1000 : qMax(1, animCfg.value(QStringLiteral("duration")).toInt());
        m_anim->setProperty("duration", dur);

        // QML: loops: (_animation.iterations === undefined || _animation.iterations < 0)
        //          ? Animation.Infinite : Math.max(1, _animation.iterations)
        // Animation.Infinite == -1
        const int iters = animCfg.isEmpty() ? -1 : animCfg.value(QStringLiteral("iterations")).toInt();
        m_anim->setProperty("loops", iters < 0 ? -1 : qMax(1, iters));

        // QML: easing.type: _animation.easing !== undefined ? _animation.easing : Easing.Linear
        // Easing.Linear == QEasingCurve::Linear == 0
        const int easingType = animCfg.isEmpty()
            ? static_cast<int>(QEasingCurve::Linear)
            : animCfg.value(QStringLiteral("easing")).toInt();
        QQmlProperty(m_anim.data(), QStringLiteral("easing.type")).write(easingType);

        // QML: running: root._animActive
        m_anim->setProperty("running", m_animActive);
    }

    applyTransform();
}


void CssRect::applyTransform()
{
    // _animActive = m_animStops.size() >= 2 (faithful to the QML `_animActive: _animStops.length >= 2`).
    // While the @keyframes animation is live (NumberAnimation on animTick running), each tick calls
    // applyAnim() which writes _animRotate/_animScale/_animTx/_animTy (the animated branch).
    // When not active, the static style["transform"] values apply.
    // Equivalent to the QML:
    //   rotation: root._animActive ? root._animRotate : (root._staticTransform.rotate ?? 0)
    //   scale:    root._animActive ? root._animScale  : (root._staticTransform.scale  ?? 1)
    //   transform: Translate { x: root._animActive ? root._animTx : (root._staticTransform.translateX ?? 0)
    //                          y: root._animActive ? root._animTy : (root._staticTransform.translateY ?? 0) }
    setRotation(m_animActive ? m_animRotate : m_staticRotate);
    setScale(m_animActive ? m_animScale : m_staticScale);
    const qreal wantTx = m_animActive ? m_animTx : m_staticTx;
    const qreal wantTy = m_animActive ? m_animTy : m_staticTy;
    if (!m_translate && (wantTx != 0.0 || wantTy != 0.0))
        ensureTranslate(); // lazy: most boxes never translate
    if (m_translate) {
        m_translate->setProperty("x", m_animActive ? m_animTx : m_staticTx);
        m_translate->setProperty("y", m_animActive ? m_animTy : m_staticTy);
    }
}

void CssRect::layoutChildren()
{
    const qreal w = width();
    const qreal h = height();
    for (QQuickItem *child : {m_render.data(), m_fastRect.data(), m_flickable ? m_flickable.data() : m_contentHolder.data()}) {
        if (!child)
            continue;
        child->setX(0);
        child->setY(0);
        child->setWidth(w);
        child->setHeight(h);
    }
    if (m_flickable && m_contentHolder) {
        // The holder becomes the scrolled content: full width, height = children extent.
        m_contentHolder->setWidth(w);
        syncScrollContent();
    }
}

void CssRect::ensureScrollable()
{
    if (m_flickable || !m_contentHolder || !isComponentComplete())
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-flickable"),
        "import QtQuick\n"
        "Flickable {\n"
        "    id: flick\n"
        "    clip: true\n"
        "    boundsBehavior: Flickable.StopAtBounds\n"
        "    flickableDirection: Flickable.VerticalFlick\n"
        // Controls-free scroll indicator on the VIEWPORT layer (parent: flick keeps it out
        // of the reparented content): sized by the visible ratio, fades in while moving.
        "    Rectangle {\n"
        "        parent: flick\n"
        "        x: flick.width - 5\n"
        "        y: flick.visibleArea.yPosition * flick.height\n"
        "        width: 3\n"
        "        radius: 1.5\n"
        "        color: \"#66000000\"\n"
        "        height: Math.max(20, flick.visibleArea.heightRatio * flick.height)\n"
        "        visible: flick.contentHeight > flick.height + 1\n"
        "        opacity: (flick.moving || flick.dragging || flick.flicking) ? 0.85 : 0.0\n"
        "        Behavior on opacity { NumberAnimation { duration: 300 } }\n"
        "    }\n"
        "}");
    QObject *o = comp->create(qmlContext(this));
    QQuickItem *flick = qobject_cast<QQuickItem *>(o);
    if (!flick) {
        if (o)
            o->deleteLater();
        qWarning("CssRect: failed to compose Flickable: %s", qPrintable(comp->errorString()));
        return;
    }
    flick->setParent(this);
    flick->setParentItem(this);
    m_flickable = flick;
    // Move the content INTO the flickable; the layout keeps operating on the same holder.
    QQuickItem *contentItem = flick->property("contentItem").value<QQuickItem *>();
    m_contentHolder->setParentItem(contentItem ? contentItem : flick);
    // Content extent follows the laid-out children.
    connect(m_contentHolder, &QQuickItem::childrenRectChanged, this, &CssRect::syncScrollContent);
    layoutChildren();
}

void CssRect::syncScrollContent()
{
    if (!m_flickable || !m_contentHolder)
        return;
    const QRectF r = m_contentHolder->childrenRect();
    // Bottom padding keeps the scroll end breathing like the web (children start offset ~= top padding).
    const qreal pad = std::max<qreal>(0.0, r.y());
    const qreal contentH = std::max(height(), r.y() + r.height() + pad);
    m_contentHolder->setHeight(contentH);
    m_flickable->setProperty("contentWidth", width());
    m_flickable->setProperty("contentHeight", contentH);
}

void CssRect::componentComplete()
{
    QQuickItem::componentComplete();

    setTransformOrigin(QQuickItem::Center);

    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    QElapsedTimer mountTimer;
    if (g_mountStats.enabled)
        mountTimer.start();

    // SINGLE-SHOT mount: resolve + apply the style BEFORE the render subtree exists.
    // recompute() no-ops while m_render is null, so the shell's very first evaluation
    // already reads the FINAL values — one pass, no default-then-restyle double work.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);

    const qint64 tResolve = g_mountStats.enabled ? mountTimer.nsecsElapsed() : 0;

    // The render shell is composed LAZILY in ensureRenderShell(): a layout-only box (no
    // background/border/shadow — most of a page) never pays for the paint machinery.

    // React to implicit-size / visibility changes like the QML on*Changed handlers.
    connect(this, &QQuickItem::implicitWidthChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::implicitHeightChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    // An ancestor's inherited text props re-propagate to us (CSS inheritance).
    if (QObject *anc = cssInheritingAncestor(this))
        connect(anc, SIGNAL(inheritedChanged()), this, SIGNAL(inheritedChanged()));

    const qint64 tShell = g_mountStats.enabled ? mountTimer.nsecsElapsed() : 0;

    layoutChildren();
    recompute(); // first and only full evaluation — the style is already final (see above)

    requestRelayout();
    if (g_mountStats.enabled) {
        const qint64 tEnd = mountTimer.nsecsElapsed();
        g_mountStats.resolveNs += tResolve;
        g_mountStats.shellNs += tShell - tResolve;
        g_mountStats.restNs += tEnd - tShell;
        ++g_mountStats.count;
    }
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssRect::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChildren();
    if (newGeometry.size() != oldGeometry.size())
        requestRelayout();
}

bool CssRect::animateGeometry(QLatin1String prop, qreal target)
{
    const bool isW = (prop == QLatin1String("width"));
    if (!transitionCovers(prop) || !isComponentComplete())
        return false;
    QVariantAnimation *&anim = isW ? m_widthAnim : m_heightAnim;
    qreal &animTarget = isW ? m_widthAnimTarget : m_heightAnimTarget;
    const qreal current = isW ? width() : height();
    // Already heading (or arrived) there: the layout re-asks every pass while children reflow —
    // treat as handled so the direct write doesn't cut the animation short.
    if (anim && anim->state() == QAbstractAnimation::Running && qFuzzyCompare(animTarget, target))
        return true;
    if (qFuzzyCompare(current, target))
        return true;
    if (!anim) {
        anim = new QVariantAnimation(this);
        if (isW)
            connect(anim, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant &v) { setWidth(v.toReal()); });
        else
            connect(anim, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant &v) { setHeight(v.toReal()); });
    }
    anim->stop();
    anim->setStartValue(current);
    anim->setEndValue(target);
    anim->setDuration(m_transMs);
    anim->setEasingCurve(static_cast<QEasingCurve::Type>(m_transEasing));
    animTarget = target;
    anim->start();
    return true;
}

void CssRect::setCssHoverStyled(bool v)
{
    if (m_hoverStyled == v)
        return;
    m_hoverStyled = v;
    emit cssHoverStyledChanged();
    if (v && !m_hoverHandler && isComponentComplete()) {
        // Compose a REAL QtQuick HoverHandler via the type-system (approved pattern; no
        // private headers). It writes cssEngineHover, whose notify re-applies the style.
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-3e8916a3"),
                "import QtQuick\nHoverHandler {}");
            if (QObject *o = comp->create(qmlContext(this))) {
                o->setParent(this);
                o->setProperty("parent", QVariant::fromValue<QObject *>(this)); // handler target
                connect(o, SIGNAL(hoveredChanged()), this, SLOT(onEngineHoverChanged()));
                m_hoverHandler = o;
            }
        }
    }
    // Losing the rule (restyle/class change): stop reporting a stale hover.
    if (!v && m_engineHover)
        setCssEngineHover(false);
}

void CssRect::setCssEngineHover(bool v)
{
    if (m_engineHover == v)
        return;
    m_engineHover = v;
    emit cssStateChanged(); // rides the state notify: the theme re-applies us + descendants
}

void CssRect::onEngineHoverChanged()
{
    if (m_hoverHandler)
        setCssEngineHover(m_hoverHandler->property("hovered").toBool());
}

void CssRect::ensureTranslate()
{
    if (m_translate || !isComponentComplete())
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-translate"),
        "import QtQuick\nTranslate {}");
    if (QObject *o = comp->create(qmlContext(this))) {
        o->setParent(this);
        QQmlListReference ref(this, "transform");
        if (ref.isValid())
            ref.append(o);
        m_translate = o;
    }
}

void CssRect::ensureAnim()
{
    if (m_anim || !isComponentComplete())
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-numberanim"),
        "import QtQuick\nNumberAnimation { from: 0.0; to: 1.0 }\n");
    if (QObject *o = comp->create(qmlContext(this))) {
        o->setParent(this);
        o->setProperty("target", QVariant::fromValue(static_cast<QObject *>(this)));
        o->setProperty("property", QStringLiteral("animTick"));
        m_anim = o;
    }
}


void CssRect::ensureRenderShell()
{
    if (m_render || !isComponentComplete())
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    // The REAL QtQuick render subtree (Shapes + MultiEffect), via the type-system.
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-shell"), kRenderShell);
    if (QObject *o = comp->create(qmlContext(this))) {
        if (QQuickItem *render = qobject_cast<QQuickItem *>(o)) {
            render->setParent(this);
            render->setParentItem(this);
            // Keep content ABOVE the fill: the render subtree paints first (lower). A
            // scrollable box holds its content inside the composed Flickable.
            QQuickItem *contentAnchor = m_flickable ? m_flickable.data() : m_contentHolder.data();
            if (contentAnchor)
                render->stackBefore(contentAnchor);
            m_render = render;
            layoutChildren(); // size the fresh shell to the current box
        } else {
            o->deleteLater();
        }
    } else {
        qWarning("CssRect: failed to compose render subtree: %s", qPrintable(comp->errorString()));
    }
}

void CssRect::ensureFastRect()
{
    if (m_fastRect || !isComponentComplete())
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    // The REAL QtQuick Rectangle — the scene graph's cheap, batchable rect node. Color and
    // border fades keep the CSS `transition` semantics of the Shape path.
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssrect-fastrect"),
        "import QtQuick\n"
        "Rectangle {\n"
        "    id: fr\n"
        "    property int transitionMs: 0\n"
        "    property int transitionEasing: Easing.InOutQuad\n"
        "    color: \"transparent\"\n"
        "    Behavior on color { enabled: fr.transitionMs > 0; ColorAnimation { duration: fr.transitionMs; easing.type: fr.transitionEasing } }\n"
        "    Behavior on border.color { enabled: fr.transitionMs > 0; ColorAnimation { duration: fr.transitionMs; easing.type: fr.transitionEasing } }\n"
        "}\n");
    if (QObject *o = comp->create(qmlContext(this))) {
        if (QQuickItem *rect = qobject_cast<QQuickItem *>(o)) {
            rect->setParent(this);
            rect->setParentItem(this);
            QQuickItem *contentAnchor = m_flickable ? m_flickable.data() : m_contentHolder.data();
            if (contentAnchor)
                rect->stackBefore(contentAnchor);
            m_fastRect = rect;
            layoutChildren();
        } else {
            o->deleteLater();
        }
    } else {
        qWarning("CssRect: failed to compose fast Rectangle: %s", qPrintable(comp->errorString()));
    }
}

void CssRect::pushFastRect()
{
    if (!m_fastRect)
        return;
    auto str = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };
    auto has = [&](const char *k) { return !m_style.value(QLatin1String(k)).toString().isEmpty(); };

    // Solid fill (the policy guarantees no gradient/url here).
    QColor solid;
    const QString bgc = str("background-color");
    const QString bg = str("background");
    if (!bgc.isEmpty() && m_theme)
        solid = m_theme->parseColor(bgc);
    else if (!bg.isEmpty() && m_theme)
        solid = m_theme->parseColor(bg);
    if (!solid.isValid())
        solid = m_defaultColor;
    m_fastRect->setProperty("transitionMs", m_transMs);
    m_fastRect->setProperty("transitionEasing", m_transEasing);
    m_fastRect->setProperty("color", solid.isValid() ? solid : QColor(Qt::transparent));

    // Radii: uniform -> radius; mixed -> per-corner (Rectangle, Qt >= 6.7).
    qreal radii[4];
    parseRadiiPx(str("border-radius"), m_radius, radii);
    const bool uniform = qFuzzyCompare(radii[0], radii[1]) && qFuzzyCompare(radii[1], radii[2])
        && qFuzzyCompare(radii[2], radii[3]);
    if (uniform) {
        m_fastRect->setProperty("radius", radii[0]);
        m_fastRect->setProperty("topLeftRadius", QVariant());
        m_fastRect->setProperty("topRightRadius", QVariant());
        m_fastRect->setProperty("bottomRightRadius", QVariant());
        m_fastRect->setProperty("bottomLeftRadius", QVariant());
    } else {
        m_fastRect->setProperty("topLeftRadius", radii[0]);
        m_fastRect->setProperty("topRightRadius", radii[1]);
        m_fastRect->setProperty("bottomRightRadius", radii[2]);
        m_fastRect->setProperty("bottomLeftRadius", radii[3]);
    }

    // Uniform border (per-side borders take the Shape path).
    QVariantMap borderShorthand;
    if (m_theme && has("border"))
        borderShorthand = m_theme->parseBorder(str("border"));
    QColor borderColor;
    if (has("border-color") && m_theme)
        borderColor = m_theme->parseColor(str("border-color"));
    else if (borderShorthand.contains(QStringLiteral("color")))
        borderColor = borderShorthand.value(QStringLiteral("color")).value<QColor>();
    else
        borderColor = m_defaultBorderColor;
    qreal borderWidth;
    if (has("border-width"))
        borderWidth = parseLeadingFloat(str("border-width"), m_defaultBorderWidth);
    else if (borderShorthand.contains(QStringLiteral("width")))
        borderWidth = borderShorthand.value(QStringLiteral("width")).toReal();
    else
        borderWidth = m_defaultBorderWidth;
    const bool borderOn = borderWidth > 0 && borderColor.alphaF() > 0;
    QObject *border = qvariant_cast<QObject *>(m_fastRect->property("border"));
    if (border) {
        border->setProperty("width", borderOn ? borderWidth : 0.0);
        border->setProperty("color", borderOn ? borderColor : QColor(Qt::transparent));
    }
}

