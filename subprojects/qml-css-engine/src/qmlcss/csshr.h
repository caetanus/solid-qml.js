#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>

class CssTheme;
class CssLayoutEngine;

// Native <hr>, translated 1:1 from qml/qmlcss/CssHr.qml — BY COMPOSITION, not reimplementation.
// A CSS-addressable Qt Quick item carrying the standard CSS signature (cssId/cssAlternateId/
// cssClass/cssState/cssPrimitive/cssPart/style) that paints only its top border. It does NOT
// paint anything itself: it instantiates a REAL QtQuick Shape via the Qt type-system and
// manages that child's geometry (the C++ equivalent of the QML anchors top/left/right). The
// Shape (a ShapePath filling a PathRectangle) is what draws the border line — Rectangle is
// avoided per the owner's rule (it is capped for several effects); Shape is the general path.
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Shapes
//   Item {                              // <- this CssHr
//       Shape {                         // <- m_shape, full-width, pinned to the top
//           width: root.width; height: Math.max(1, root.line.width)
//           visible: root.line.visible
//           ShapePath {
//               strokeWidth: 0; strokeColor: "transparent"
//               fillColor: root.line.color
//               PathRectangle { x: 0; y: 0; width: shape.width; height: shape.height }
//           }
//       }
//   }
class CssHr : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)
    Q_PROPERTY(QVariantMap line READ line NOTIFY lineChanged)

public:
    explicit CssHr(QQuickItem *parent = nullptr);

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
    QVariantMap line() const { return m_line; }

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssStateChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void styleChanged();
    void hasCssIdentityChanged();
    void lineChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Re-run cssTheme.borderSide(style, "top", 1, "#ededed"), push into the Rectangle child,
    // refresh implicitHeight, and emit lineChanged. Faithful to the QML `line` binding.
    void recomputeLine();
    // top/left/right anchor equivalent: keep the child pinned full-width at the top.
    void layoutChild();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss();

    QString m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass = QVariant::fromValue(QVariantList());
    QVariant m_cssState = QVariant::fromValue(QVariantList());
    QString m_cssPrimitive = QStringLiteral("hr");
    QString m_cssPart;
    QVariantMap m_style;
    QVariantMap m_line;

    QPointer<CssTheme> m_theme;
    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_shape; // the REAL QtQuick Shape (ShapePath + PathRectangle), via the type-system
    bool m_displayHidden = false; // we hid via display:none (so only we restore visible)
};
