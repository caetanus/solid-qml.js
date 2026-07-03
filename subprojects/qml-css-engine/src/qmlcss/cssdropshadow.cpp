#include "qmlcss/componentcache.h"
#include "qmlcss/cssdropshadow.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>

#include <algorithm>

// Equivalent QML (see cssdropshadow.h for the annotated version):
//   import QtQuick.Effects
//   MultiEffect {
//       source: root.source
//       shadowEnabled: hasShadow
//       shadowColor: hasShadow ? shadow.color : "transparent"
//       shadowHorizontalOffset: hasShadow ? shadow.x : 0
//       shadowVerticalOffset: hasShadow ? shadow.y : 0
//       shadowBlur: hasShadow ? Math.min(1, shadow.blur / 32) : 0
//       autoPaddingEnabled: true
//   }

CssDropShadow::CssDropShadow(QQuickItem *parent)
    : QQuickItem(parent)
{
}

bool CssDropShadow::hasShadow() const
{
    // QML: readonly property bool hasShadow: shadow && shadow.color !== undefined
    return m_shadow.contains(QStringLiteral("color"));
}

void CssDropShadow::setSource(const QVariant &v)
{
    if (m_source == v)
        return;
    m_source = v;
    // QML: source: root.source (alias on the root MultiEffect)
    if (m_effect)
        m_effect->setProperty("source", m_source);
    emit sourceChanged();
}

void CssDropShadow::setShadow(const QVariantMap &v)
{
    const bool wasHas = hasShadow();
    if (m_shadow == v)
        return;
    m_shadow = v;
    emit shadowChanged();
    if (wasHas != hasShadow())
        emit hasShadowChanged();
    recomputeShadow();
}

void CssDropShadow::recomputeShadow()
{
    if (!m_effect)
        return;
    const bool has = hasShadow();
    // QML: shadowEnabled: hasShadow
    m_effect->setProperty("shadowEnabled", has);
    // QML: shadowColor: hasShadow ? shadow.color : "transparent"
    m_effect->setProperty("shadowColor",
                          has ? m_shadow.value(QStringLiteral("color")).value<QColor>()
                              : QColor(Qt::transparent));
    // QML: shadowHorizontalOffset: hasShadow ? shadow.x : 0
    m_effect->setProperty("shadowHorizontalOffset",
                          has ? m_shadow.value(QStringLiteral("x")).toReal() : 0.0);
    // QML: shadowVerticalOffset: hasShadow ? shadow.y : 0
    m_effect->setProperty("shadowVerticalOffset",
                          has ? m_shadow.value(QStringLiteral("y")).toReal() : 0.0);
    // QML: shadowBlur: hasShadow ? Math.min(1, shadow.blur / 32) : 0
    m_effect->setProperty("shadowBlur",
                          has ? std::min<qreal>(1.0, m_shadow.value(QStringLiteral("blur")).toReal() / 32.0)
                              : 0.0);
}

void CssDropShadow::layoutEffect()
{
    // C++ equivalent of `anchors.fill: parent` on the composed MultiEffect.
    if (!m_effect)
        return;
    m_effect->setX(0);
    m_effect->setY(0);
    m_effect->setWidth(width());
    m_effect->setHeight(height());
}

void CssDropShadow::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlEngine *eng = qmlEngine(this)) {
        // The REAL QtQuick.Effects MultiEffect — the actual renderer. autoPaddingEnabled matches
        // the QML original so the shadow is never clipped by this item's bounds.
        QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("cssdropshadow-2001d9c5"),
            
            "import QtQuick.Effects\n"
            "MultiEffect { autoPaddingEnabled: true }\n");
        if (QObject *o = comp->create(qmlContext(this))) {
            if (QQuickItem *fx = qobject_cast<QQuickItem *>(o)) {
                fx->setParentItem(this);
                m_effect = fx;
            } else {
                o->deleteLater();
            }
        }
    }

    // Forward any source set before componentComplete.
    if (m_effect && !m_source.isNull())
        m_effect->setProperty("source", m_source);

    recomputeShadow();
    layoutEffect();
}

void CssDropShadow::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // Keep the composed MultiEffect filling us (C++ anchors.fill equivalent).
    layoutEffect();
}
