#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

class CssTheme;
class CssLayoutEngine;

// Native <img>, translated 1:1 from qml/qmlcss/CssImage.qml — BY COMPOSITION, not reimplementation.
// A CSS-addressable Qt Quick item that shows a source image. It does NOT paint anything itself:
// it instantiates the REAL QtQuick Image (via the Qt type-system) and, for rounded corners, the
// REAL QtQuick.Effects MultiEffect (the cross-backend shader/mask idiom) whose mask source is a
// REAL QtQuick Shape rounded-rect (Rectangle is avoided per the owner's rule). `object-fit` maps
// to Image.fillMode; a non-zero `border-radius` rounds the image through the MultiEffect mask
// (so `border-radius: 50%` yields a real circle). Without radius, the Image draws direct.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/
// cssPart/style); resolved rules are pushed back into `style` by cssTheme.loadCss(this).
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Effects
//   import QtQuick.Shapes
//   Item {                                        // <- this CssImage
//       Image {                                   // <- m_image
//           anchors.fill: parent
//           fillMode: root._fillMode              // object-fit
//           asynchronous: true; cache: true
//           visible: !root._rounded
//           layer.enabled: root._rounded
//       }
//       MultiEffect {                             // <- m_effect (only when rounded)
//           anchors.fill: img; source: img
//           visible: root._rounded
//           maskEnabled: true
//           maskSource: mask
//       }
//       Item {                                    // <- m_maskHolder (layered, hidden)
//           id: mask; anchors.fill: img; layer.enabled: true; visible: false
//           Shape {                               // <- m_mask, rounded-rect mask
//               anchors.fill: parent
//               ShapePath {
//                   strokeWidth: 0; fillColor: "white"
//                   PathRectangle { width: mask.width; height: mask.height; radius: root._radius }
//               }
//           }
//       }
//   }
class CssImage : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)
    // The image URL (QML: property alias source: img.source).
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit CssImage(QQuickItem *parent = nullptr);

    QString cssId() const { return m_cssId; }
    void setCssId(const QString &v);

    QVariant cssAlternateId() const { return m_cssAlternateId; }
    void setCssAlternateId(const QVariant &v);

    QVariant cssClass() const { return m_cssClass; }
    void setCssClass(const QVariant &v);

    QVariant cssState() const { return m_cssState; }
    void setCssState(const QVariant &v);

    QString cssPrimitive() const { return m_cssPrimitive; }
    void setCssPrimitive(const QString &v);

    QString cssPart() const { return m_cssPart; }
    void setCssPart(const QString &v);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    bool hasCssIdentity() const;

    QUrl source() const { return m_source; }
    void setSource(const QUrl &v);

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssStateChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void styleChanged();
    void hasCssIdentityChanged();
    void sourceChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Recompute fillMode (object-fit), radius/rounded (border-radius) and push them onto the
    // composed Image / MultiEffect / mask Rectangle — the C++ equivalent of the QML bindings.
    void recompute();
    // anchors.fill equivalent: keep every composed child sized to us.
    void layoutChildren();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss();

    QString m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass = QVariant::fromValue(QVariantList());
    QVariant m_cssState = QVariant::fromValue(QVariantList());
    QString m_cssPrimitive = QStringLiteral("img");
    QString m_cssPart;
    QVariantMap m_style;
    QUrl m_source;

    QPointer<CssTheme> m_theme;
    QPointer<QQuickItem> m_image;  // REAL QtQuick Image
    QPointer<QQuickItem> m_effect; // REAL QtQuick.Effects MultiEffect (rounded-corner mask)
    QPointer<QQuickItem> m_mask;   // REAL QtQuick Shape (rounded-rect) used as the MultiEffect maskSource
    bool m_displayHidden = false; // we hid via display:none (so only we restore visible)
};
