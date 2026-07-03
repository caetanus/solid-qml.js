#include "qmlcss/cssimage.h"

#include "qmlcss/csstheme.h"

#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>

// Equivalent QML this C++ composes (see cssimage.h for the annotated version):
//
//   import QtQuick
//   import QtQuick.Effects
//   import QtQuick.Shapes
//   Item {                                        // <- this CssImage
//       Image { anchors.fill: parent; fillMode: root._fillMode; asynchronous: true; cache: true
//               visible: !root._rounded }
//       MultiEffect { anchors.fill: img; source: img; visible: root._rounded
//                     maskEnabled: root._rounded; maskSource: mask }
//       Shape { id: mask; anchors.fill: img; layer.enabled: true         // rounded-rect mask
//               ShapePath { strokeWidth: 0; fillColor: "white"
//                           PathRectangle { width: mask.width; height: mask.height
//                                           radius: root._radius } } }
//   }

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

// QML: switch on object-fit -> Image.fillMode. Numeric values are QQuickImage::FillMode.
// Stretch=0, PreserveAspectFit=1, PreserveAspectCrop=2, Tile=3, ..., Pad=6.
int fillModeFor(const QString &objectFit)
{
    if (objectFit == QLatin1String("contain"))
        return 1; // PreserveAspectFit
    if (objectFit == QLatin1String("fill"))
        return 0; // Stretch
    if (objectFit == QLatin1String("none"))
        return 6; // Pad
    if (objectFit == QLatin1String("scale-down"))
        return 1; // PreserveAspectFit
    return 2;     // cover -> PreserveAspectCrop
}

} // namespace

CssImage::CssImage(QQuickItem *parent)
    : QQuickItem(parent)
{
}

bool CssImage::hasCssIdentity() const
{
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    return !toStringList(m_cssClass).isEmpty();
}

void CssImage::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssImage::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssImage::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssClassChanged -> cssTheme.loadCss(this)
}

void CssImage::setCssState(const QVariant &v)
{
    if (m_cssState == v)
        return;
    m_cssState = v;
    emit cssStateChanged();
}

void CssImage::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
}

void CssImage::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
}

void CssImage::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();

    // Only a DECLARED display drives visible (an unconditional write severs author bindings).
    const QString display = m_style.value(QStringLiteral("display")).toString();
    if (display == QLatin1String("none")) {
        setVisible(false);
        m_displayHidden = true;
    } else if (m_displayHidden) {
        setVisible(true);
        m_displayHidden = false;
    }

    // QML: _fillMode / _radius / _rounded bindings all read `style` -> re-push onto the children.
    recompute();
}

void CssImage::setSource(const QUrl &v)
{
    if (m_source == v)
        return;
    m_source = v;
    if (m_image)
        m_image->setProperty("source", m_source); // QML: alias source: img.source
    emit sourceChanged();
}

void CssImage::recompute()
{
    // QML: _objectFit = style["object-fit"] || "cover"
    const QString objectFit = m_style.value(QStringLiteral("object-fit")).toString().isEmpty()
        ? QStringLiteral("cover")
        : m_style.value(QStringLiteral("object-fit")).toString();
    const int fillMode = fillModeFor(objectFit);

    // QML: _radius = cssTheme.parseLength(style["border-radius"], 0); _rounded = _radius > 0.5
    qreal radius = 0;
    const QString radiusStr = m_style.value(QStringLiteral("border-radius")).toString();
    if (m_theme && !radiusStr.isEmpty())
        radius = m_theme->parseLength(radiusStr, 0);
    const bool rounded = radius > 0.5;

    if (m_image) {
        m_image->setProperty("fillMode", fillMode);
        // QML: img.visible: !root._rounded  (rounded -> drawn by the MultiEffect through the mask)
        m_image->setVisible(!rounded);
    }
    if (m_mask)
        m_mask->setProperty("maskRadius", radius);
    if (m_effect) {
        // QML: MultiEffect { visible: rounded; maskEnabled: rounded }
        m_effect->setVisible(rounded);
        m_effect->setProperty("maskEnabled", rounded);
    }
}

void CssImage::layoutChildren()
{
    // C++ equivalent of anchors.fill: keep every composed child sized to us.
    const qreal w = width();
    const qreal h = height();
    for (QQuickItem *child : {m_image.data(), m_effect.data(), m_mask.data()}) {
        if (!child)
            continue;
        child->setX(0);
        child->setY(0);
        child->setWidth(w);
        child->setHeight(h);
    }
    if (m_mask) {
        // Drive the mask's PathRectangle (its aliases) so the rounded rect fills the item.
        m_mask->setProperty("maskWidth", w);
        m_mask->setProperty("maskHeight", h);
    }
}

void CssImage::maybeLoadCss()
{
    // QML: if (cssTheme && root._hasCssIdentity) cssTheme.loadCss(this)
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

void CssImage::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlContext *ctx = qmlContext(this))
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());

    QQmlEngine *eng = qmlEngine(this);
    if (eng) {
        // REAL QtQuick Image (a native texture provider, so MultiEffect can consume it directly).
        {
            QQmlComponent comp(eng);
            comp.setData("import QtQuick\nImage { asynchronous: true; cache: true }", QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *img = qobject_cast<QQuickItem *>(o)) {
                    img->setParentItem(this);
                    m_image = img;
                } else {
                    o->deleteLater();
                }
            }
        }

        // REAL QtQuick Shape rounded-rect used as the MultiEffect mask (Rectangle avoided per rule).
        // layer.enabled makes the Shape a texture provider so it can be a maskSource.
        {
            QQmlComponent comp(eng);
            comp.setData(
                "import QtQuick\n"
                "import QtQuick.Shapes\n"
                "Shape {\n"
                "    property alias maskWidth: pr.width\n"
                "    property alias maskHeight: pr.height\n"
                "    property alias maskRadius: pr.radius\n"
                "    layer.enabled: true\n"
                "    visible: false\n"
                "    ShapePath {\n"
                "        strokeWidth: 0\n"
                "        strokeColor: \"transparent\"\n"
                "        fillColor: \"white\"\n"
                "        PathRectangle { id: pr; x: 0; y: 0; width: 1; height: 1; radius: 0 }\n"
                "    }\n"
                "}\n",
                QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *mask = qobject_cast<QQuickItem *>(o)) {
                    mask->setParentItem(this);
                    m_mask = mask;
                } else {
                    o->deleteLater();
                }
            }
        }

        // REAL QtQuick.Effects MultiEffect: source = Image, maskSource = Shape.
        {
            QQmlComponent comp(eng);
            comp.setData("import QtQuick.Effects\nMultiEffect {}", QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *effect = qobject_cast<QQuickItem *>(o)) {
                    effect->setParentItem(this);
                    if (m_image)
                        effect->setProperty("source", QVariant::fromValue(m_image.data()));
                    if (m_mask)
                        effect->setProperty("maskSource", QVariant::fromValue(m_mask.data()));
                    m_effect = effect;
                } else {
                    o->deleteLater();
                }
            }
        }
    }

    if (m_image && m_source.isValid())
        m_image->setProperty("source", m_source);

    layoutChildren();
    recompute();

    // QML Component.onCompleted.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this); // sets `style` -> setStyle recomputes fill/radius
}

void CssImage::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChildren();
}
