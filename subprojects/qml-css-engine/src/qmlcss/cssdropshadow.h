#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>

// Reusable drop-shadow, translated 1:1 from qml/qmlcss/CssDropShadow.qml — BY COMPOSITION.
// Intended as a Text/Item `layer.effect` or sibling effect. The root QML WAS a MultiEffect;
// this C++ version is a QQuickItem that COMPOSES a REAL QtQuick.Effects MultiEffect (via the
// Qt type-system) and forwards `source` and `shadow` settings onto it.
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Effects
//   MultiEffect {                               // <- m_effect (fills this item)
//       source: root.source
//       property var shadow: ({})
//       readonly property bool hasShadow: shadow && shadow.color !== undefined
//       shadowEnabled: hasShadow
//       shadowColor: hasShadow ? shadow.color : "transparent"
//       shadowHorizontalOffset: hasShadow ? shadow.x : 0
//       shadowVerticalOffset: hasShadow ? shadow.y : 0
//       // MultiEffect.shadowBlur is 0..1 over blurMax (32 px default)
//       shadowBlur: hasShadow ? Math.min(1, shadow.blur / 32) : 0
//       autoPaddingEnabled: true
//   }
class CssDropShadow : public QQuickItem {
    Q_OBJECT
    // Source: the item (or layer texture) to apply the shadow to — forwarded to the internal
    // MultiEffect. When used as `layer.effect`, Qt Quick sets this automatically.
    Q_PROPERTY(QVariant source READ source WRITE setSource NOTIFY sourceChanged)
    // Shadow descriptor: QVariantMap with optional keys x, y, blur (px), color (QColor).
    // Mirrors the QML `property var shadow: ({})`.
    Q_PROPERTY(QVariantMap shadow READ shadow WRITE setShadow NOTIFY shadowChanged)
    // Readonly: true when shadow contains a "color" key (i.e. shadow was parsed successfully).
    Q_PROPERTY(bool hasShadow READ hasShadow NOTIFY hasShadowChanged)

public:
    explicit CssDropShadow(QQuickItem *parent = nullptr);

    QVariant source() const { return m_source; }
    void setSource(const QVariant &v);

    QVariantMap shadow() const { return m_shadow; }
    void setShadow(const QVariantMap &v);

    bool hasShadow() const;

signals:
    void sourceChanged();
    void shadowChanged();
    void hasShadowChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Push the current shadow map onto the composed MultiEffect's shadow* props.
    void recomputeShadow();
    // Keep the composed MultiEffect sized to this item (anchors.fill equivalent).
    void layoutEffect();

    QVariant m_source;
    QVariantMap m_shadow;

    QPointer<QQuickItem> m_effect; // REAL QtQuick.Effects MultiEffect
};
