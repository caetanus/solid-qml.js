#include "qmlcss/cssrect.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"

#include <QEasingCurve>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QUrl>

#include <algorithm>

namespace {

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

    // --- inputs pushed by C++ (the resolved CSS values) --------------------------------
    property string radiusStr: ""
    property real radiusFallback: 0
    property color solid: "transparent"
    property real fillAlpha: 1
    property bool hasGradient: false
    property var gradient: ({})
    property var orderedLayers: []
    property bool borderVisible: false
    property color borderColorOpaque: "transparent"
    property real borderWidth: 0
    property real borderAlpha: 1
    property bool hasSideBorder: false
    property var topB: ({})
    property var rightB: ({})
    property var bottomB: ({})
    property var leftB: ({})
    property bool hasOutsetShadow: false
    property color shadowFillColor: "#ffffff"
    property real shadowSrcOpacity: 1
    property color shadowColorOpaque: "transparent"
    property real shadowOpacity: 0
    property real shadowX: 0
    property real shadowY: 0
    property real shadowBlur: 0
    property bool insetBevel: false
    property color insetDark: "transparent"
    property color insetLight: Qt.rgba(1, 1, 1, 0.30)
    property bool insetEdge: false
    property real insetEdgeX: 0
    property real insetEdgeY: 0
    property color insetEdgeColor: "transparent"
    property int transitionMs: 0
    property int transitionEasing: Easing.InOutQuad

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
    function clampedRadius(index) {
        return Math.max(0, Math.min(r.borderRadii[index], r.width / 2, r.height / 2))
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
    Item {
        anchors.fill: parent
        visible: r.hasOutsetShadow

        Shape {
            id: shadowSource
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: r.shadowSrcOpacity
            ShapePath {
                strokeColor: "transparent"
                strokeWidth: 0
                fillColor: r.shadowFillColor
                startX: r.clampedRadius(0); startY: 0
                PathLine { x: Math.max(r.clampedRadius(0), r.width - r.clampedRadius(1)); y: 0 }
                PathArc { x: r.width; y: r.clampedRadius(1); radiusX: r.clampedRadius(1); radiusY: r.clampedRadius(1); direction: PathArc.Clockwise }
                PathLine { x: r.width; y: Math.max(r.clampedRadius(1), r.height - r.clampedRadius(2)) }
                PathArc { x: r.width - r.clampedRadius(2); y: r.height; radiusX: r.clampedRadius(2); radiusY: r.clampedRadius(2); direction: PathArc.Clockwise }
                PathLine { x: r.clampedRadius(3); y: r.height }
                PathArc { x: 0; y: r.height - r.clampedRadius(3); radiusX: r.clampedRadius(3); radiusY: r.clampedRadius(3); direction: PathArc.Clockwise }
                PathLine { x: 0; y: r.clampedRadius(0) }
                PathArc { x: r.clampedRadius(0); y: 0; radiusX: r.clampedRadius(0); radiusY: r.clampedRadius(0); direction: PathArc.Clockwise }
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

    // --- the fill (solid colour or linear gradient). Alpha rides on opacity. -------------
    Shape {
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
            startX: r.clampedRadius(0); startY: 0
            PathLine { x: Math.max(r.clampedRadius(0), r.width - r.clampedRadius(1)); y: 0 }
            PathArc { x: r.width; y: r.clampedRadius(1); radiusX: r.clampedRadius(1); radiusY: r.clampedRadius(1); direction: PathArc.Clockwise }
            PathLine { x: r.width; y: Math.max(r.clampedRadius(1), r.height - r.clampedRadius(2)) }
            PathArc { x: r.width - r.clampedRadius(2); y: r.height; radiusX: r.clampedRadius(2); radiusY: r.clampedRadius(2); direction: PathArc.Clockwise }
            PathLine { x: r.clampedRadius(3); y: r.height }
            PathArc { x: 0; y: r.height - r.clampedRadius(3); radiusX: r.clampedRadius(3); radiusY: r.clampedRadius(3); direction: PathArc.Clockwise }
            PathLine { x: 0; y: r.clampedRadius(0) }
            PathArc { x: r.clampedRadius(0); y: 0; radiusX: r.clampedRadius(0); radiusY: r.clampedRadius(0); direction: PathArc.Clockwise }
        }
        LinearGradient {
            id: linearGradient
            x1: r.width / 2 - r.dirX * r.lineLen / 2
            y1: r.height / 2 - r.dirY * r.lineLen / 2
            x2: r.width / 2 + r.dirX * r.lineLen / 2
            y2: r.height / 2 + r.dirY * r.lineLen / 2
        }
    }

    // --- stacked background layers (radial / multi-layer) the single fill can't express --
    Repeater {
        model: r.orderedLayers
        CssFillLayer {
            anchors.fill: parent
            spec: modelData
            radii: [r.clampedRadius(0), r.clampedRadius(1), r.clampedRadius(2), r.clampedRadius(3)]
        }
    }

    // --- border as its own Shape (independent alpha) ------------------------------------
    Shape {
        anchors.fill: parent
        visible: r.borderVisible
        preferredRendererType: Shape.CurveRenderer
        opacity: r.borderAlpha
        ShapePath {
            strokeColor: r.borderColorOpaque
            strokeWidth: r.borderWidth
            fillColor: "transparent"
            startX: r.clampedRadius(0); startY: 0
            PathLine { x: Math.max(r.clampedRadius(0), r.width - r.clampedRadius(1)); y: 0 }
            PathArc { x: r.width; y: r.clampedRadius(1); radiusX: r.clampedRadius(1); radiusY: r.clampedRadius(1); direction: PathArc.Clockwise }
            PathLine { x: r.width; y: Math.max(r.clampedRadius(1), r.height - r.clampedRadius(2)) }
            PathArc { x: r.width - r.clampedRadius(2); y: r.height; radiusX: r.clampedRadius(2); radiusY: r.clampedRadius(2); direction: PathArc.Clockwise }
            PathLine { x: r.clampedRadius(3); y: r.height }
            PathArc { x: 0; y: r.height - r.clampedRadius(3); radiusX: r.clampedRadius(3); radiusY: r.clampedRadius(3); direction: PathArc.Clockwise }
            PathLine { x: 0; y: r.clampedRadius(0) }
            PathArc { x: r.clampedRadius(0); y: 0; radiusX: r.clampedRadius(0); radiusY: r.clampedRadius(0); direction: PathArc.Clockwise }
        }
    }

    // --- per-side CSS borders (border-top, etc.) ---------------------------------------
    Item {
        anchors.fill: parent
        visible: r.hasSideBorder
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

    // --- sunken bevel for two inset box-shadows ----------------------------------------
    Item {
        anchors.fill: parent
        visible: r.insetBevel
        Bar { // top — shadow
            visible: r.bevelEdge(0, 1)
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.leftMargin: r.clampedRadius(0); anchors.rightMargin: r.clampedRadius(1)
            height: 1; barColor: r.insetDark
        }
        Bar { // left — shadow
            visible: r.bevelEdge(0, 3)
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: r.clampedRadius(0); anchors.bottomMargin: r.clampedRadius(3)
            width: 1; barColor: r.insetDark
        }
        Bar { // bottom — highlight
            visible: r.bevelEdge(3, 2)
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: r.clampedRadius(3); anchors.rightMargin: r.clampedRadius(2)
            height: 1; barColor: r.insetLight
        }
        Bar { // right — highlight
            visible: r.bevelEdge(1, 2)
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: r.clampedRadius(1); anchors.bottomMargin: r.clampedRadius(2)
            width: 1; barColor: r.insetLight
        }
    }

    // --- a single inset box-shadow = a directional inner edge --------------------------
    Item {
        anchors.fill: parent
        visible: r.insetEdge
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

    // Opaque gradient stops; the gradient's alpha is carried on the fill Shape's opacity.
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
    onGradientChanged: rebuildStops()
    Component.onCompleted: rebuildStops()
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
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssRect::setCssState(const QVariant &v)
{
    if (m_cssState == v)
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

    const QString display = m_style.value(QStringLiteral("display")).toString();
    setVisible(display != QLatin1String("none"));

    // visibility:hidden -> painted invisible (opacity 0) and inert (enabled false), but STILL in flow.
    const bool hidden = m_style.value(QStringLiteral("visibility")).toString() == QLatin1String("hidden");
    if (hidden)
        setOpacity(0);
    else
        setOpacity(m_style.contains(QStringLiteral("opacity"))
                       ? m_style.value(QStringLiteral("opacity")).toReal()
                       : 1.0);
    setEnabled(!hidden);
    setZ(m_style.contains(QStringLiteral("z-index")) ? m_style.value(QStringLiteral("z-index")).toReal() : 0.0);
    const QString overflow = m_style.value(QStringLiteral("overflow")).toString();
    setClip(overflow == QLatin1String("hidden") || overflow == QLatin1String("clip"));

    recompute();
    requestRelayout();
    if (m_layout)
        m_layout->notifyParentLayout(this);
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
    // QML: onAnimTickChanged: if (typeof cssLayout !== "undefined" && cssLayout)
    //          cssLayout.applyAnim(root, root._animStops, root.animTick)
    if (m_layout)
        m_layout->applyAnim(this, m_animStops, v);
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

void CssRect::recompute()
{
    if (!m_render)
        return;

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

    // --- push everything onto the composed render root ---
    QQuickItem *g = m_render;
    g->setProperty("radiusStr", str("border-radius"));
    g->setProperty("radiusFallback", m_radius);
    g->setProperty("solid", opaque(solid));
    g->setProperty("fillAlpha", fillAlpha);
    g->setProperty("hasGradient", hasGradient);
    g->setProperty("gradient", gradient);
    g->setProperty("orderedLayers", orderedLayers);
    g->setProperty("borderVisible", borderVisible);
    g->setProperty("borderColorOpaque", opaque(borderColor));
    g->setProperty("borderWidth", borderWidth);
    g->setProperty("borderAlpha", borderColor.alphaF());
    g->setProperty("hasSideBorder", hasSideBorder);
    g->setProperty("topB", topB);
    g->setProperty("rightB", rightB);
    g->setProperty("bottomB", bottomB);
    g->setProperty("leftB", leftB);
    g->setProperty("hasOutsetShadow", hasOutsetShadow);
    g->setProperty("shadowFillColor", hasGradient ? QColor(QStringLiteral("#ffffff")) : opaque(solid));
    g->setProperty("shadowSrcOpacity", hasGradient ? gradientPeakAlpha : solid.alphaF());
    g->setProperty("shadowColorOpaque", hasOutsetShadow ? opaque(outColor) : QColor(Qt::transparent));
    g->setProperty("shadowOpacity", hasOutsetShadow ? outColor.alphaF() : 0.0);
    g->setProperty("shadowX", hasOutsetShadow ? outset.value(QStringLiteral("x")).toReal() : 0.0);
    g->setProperty("shadowY", hasOutsetShadow ? outset.value(QStringLiteral("y")).toReal() : 0.0);
    g->setProperty("shadowBlur", hasOutsetShadow
                       ? std::min<qreal>(1.0, outset.value(QStringLiteral("blur")).toReal() / 32.0) : 0.0);
    g->setProperty("insetBevel", insetBevel);
    g->setProperty("insetDark", insetDark);
    g->setProperty("insetLight", insetLight);
    g->setProperty("insetEdge", insetEdge);
    g->setProperty("insetEdgeX", insetEdge ? insetEdgeShadow.value(QStringLiteral("x")).toReal() : 0.0);
    g->setProperty("insetEdgeY", insetEdge ? insetEdgeShadow.value(QStringLiteral("y")).toReal() : 0.0);
    g->setProperty("insetEdgeColor", insetEdge ? insetEdgeShadow.value(QStringLiteral("color")).value<QColor>()
                                               : QColor(Qt::transparent));
    g->setProperty("transitionMs", transitionMs);
    g->setProperty("transitionEasing", transitionEasing);

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
    if (m_translate) {
        m_translate->setProperty("x", m_animActive ? m_animTx : m_staticTx);
        m_translate->setProperty("y", m_animActive ? m_animTy : m_staticTy);
    }
}

void CssRect::layoutChildren()
{
    const qreal w = width();
    const qreal h = height();
    for (QQuickItem *child : {m_render.data(), m_contentHolder.data()}) {
        if (!child)
            continue;
        child->setX(0);
        child->setY(0);
        child->setWidth(w);
        child->setHeight(h);
    }
}

void CssRect::componentComplete()
{
    QQuickItem::componentComplete();

    setTransformOrigin(QQuickItem::Center);

    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    if (QQmlEngine *eng = qmlEngine(this)) {
        // The REAL QtQuick render subtree (Shapes + MultiEffect), via the type-system.
        {
            QQmlComponent comp(eng);
            comp.setData(kRenderShell, QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *render = qobject_cast<QQuickItem *>(o)) {
                    render->setParentItem(this);
                    // Keep content ABOVE the fill: the render subtree paints first (lower).
                    if (m_contentHolder)
                        render->stackBefore(m_contentHolder);
                    m_render = render;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssRect: failed to compose render subtree: %s", qPrintable(comp.errorString()));
            }
        }

        // A REAL QtQuick Translate appended to our transform list (static/animated translate).
        {
            QQmlComponent comp(eng);
            comp.setData("import QtQuick\nTranslate {}", QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                QQmlListReference ref(this, "transform");
                if (ref.isValid())
                    ref.append(o);
                m_translate = o;
            }
        }

        // A REAL QtQuick NumberAnimation on our `animTick` property (0→1), driving the @keyframes
        // interpolation. Created once here; reconfigured (duration/loops/easing/running) in every
        // recompute() when the style changes. Equivalent QML:
        //   NumberAnimation on animTick { from: 0.0; to: 1.0; ... running: root._animActive }
        {
            QQmlComponent comp(eng);
            comp.setData(
                "import QtQuick\n"
                "NumberAnimation { from: 0.0; to: 1.0 }\n",
                QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                o->setParent(this);
                o->setProperty("target", QVariant::fromValue(static_cast<QObject *>(this)));
                o->setProperty("property", QStringLiteral("animTick"));
                m_anim = o;
            } else {
                qWarning("CssRect: failed to compose anim NumberAnimation: %s",
                         qPrintable(comp.errorString()));
            }
        }
    }

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

    layoutChildren();
    recompute();

    // QML Component.onCompleted.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
    requestRelayout();
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
