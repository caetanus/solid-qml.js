#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>

// CSS icon widget, translated 1:1 from qml/qmlcss/CssIcon.qml — BY COMPOSITION.
// Renders an icon image (from a CSS `icon`/`icon-image`/`icon-name` prop, a fallback source
// or a theme-icon name) with optional colour tinting, by composing a REAL QtQuick Image and a
// REAL QtQuick.Effects MultiEffect (via the Qt type-system). The Image is hidden; the
// MultiEffect is the visible renderer so the colorization shader is always active.
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Effects
//   Item {                                       // <- this CssIcon
//       property var style: ({})
//       property string fallbackSource: ""
//       property string fallbackIconName: ""
//       property color color: "white"
//       property bool colorize: true
//       property int iconSize: Math.min(width, height)   // auto unless set explicitly
//       readonly property bool ready: iconImage.status === Image.Ready
//
//       // Source resolution (literal JS port):
//       //   cssIconName  -> "image://themeicon/<name>|<hexcolor-without-#>"
//       //   cssIconValue -> sourceFromCssUrl(value)
//       //   fallbackIconName -> themeicon URL
//       //   else -> fallbackSource
//
//       Image {                                  // <- m_image (source provider)
//           id: iconImage
//           anchors.centerIn: parent
//           width: root.iconSize; height: root.iconSize
//           sourceSize.width: root.iconSize      // grouped — set via QQmlProperty
//           sourceSize.height: root.iconSize
//           source: root.iconSource
//           fillMode: Image.PreserveAspectFit    // == 1
//           smooth: true; mipmap: true
//           visible: false
//       }
//       MultiEffect {                            // <- m_effect (visible renderer)
//           anchors.fill: iconImage
//           source: iconImage
//           visible: iconImage.status === Image.Ready
//           colorization: root.colorize ? 1.0 : 0.0
//           colorizationColor: root.color
//       }
//   }
class CssIcon : public QQuickItem {
    Q_OBJECT
    // CSS style map; keys `icon`, `icon-image`, `icon-name` drive the source URL.
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    // Fallback URL used when no CSS icon is resolved.
    Q_PROPERTY(QString fallbackSource READ fallbackSource WRITE setFallbackSource NOTIFY fallbackSourceChanged)
    // Fallback theme-icon name (same resolution path as `icon-name`).
    Q_PROPERTY(QString fallbackIconName READ fallbackIconName WRITE setFallbackIconName NOTIFY fallbackIconNameChanged)
    // Tint colour applied by the MultiEffect colorization shader (default white = no tint).
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // Whether colorization is active (default true — colorize with `color`).
    Q_PROPERTY(bool colorize READ colorize WRITE setColorize NOTIFY colorizeChanged)
    // Square size of the icon, in pixels. Defaults to min(width, height); explicitly set
    // to override. Recomputed in geometryChange unless set explicitly.
    Q_PROPERTY(int iconSize READ iconSize WRITE setIconSize NOTIFY iconSizeChanged)
    // Readonly: true when the composed Image reports Image.Ready (status == 1).
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit CssIcon(QQuickItem *parent = nullptr);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    QString fallbackSource() const { return m_fallbackSource; }
    void setFallbackSource(const QString &v);

    QString fallbackIconName() const { return m_fallbackIconName; }
    void setFallbackIconName(const QString &v);

    QColor color() const { return m_color; }
    void setColor(const QColor &v);

    bool colorize() const { return m_colorize; }
    void setColorize(bool v);

    int iconSize() const { return m_iconSize; }
    void setIconSize(int v);

    bool ready() const;

signals:
    void styleChanged();
    void fallbackSourceChanged();
    void fallbackIconNameChanged();
    void colorChanged();
    void colorizeChanged();
    void iconSizeChanged();
    void readyChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Resolve the icon URL from style / fallbackIconName / fallbackSource.
    // Literal port of the QML `iconSource` binding and `sourceFromCssUrl(value)` function.
    QString resolveIconSource() const;
    QString sourceFromCssUrl(const QString &value) const;

    // Push the resolved source URL into the composed Image.
    void updateIconSource();
    // Push colorization props into the composed MultiEffect.
    void updateColorize();
    // Update m_iconSize from min(width, height) when not explicitly set; notify + relayout.
    void updateAutoIconSize();
    // Keep Image centered + sized to iconSize; MultiEffect fills Image.
    void layoutChildren();

private slots:
    // Connected to Image::statusChanged — update MultiEffect visibility and emit readyChanged.
    void onImageStatusChanged();

private:
    QVariantMap m_style;
    QString m_fallbackSource;
    QString m_fallbackIconName;
    QColor m_color = Qt::white;
    bool m_colorize = true;
    int m_iconSize = 0;
    bool m_iconSizeExplicit = false; // true once setIconSize was called directly

    QPointer<QQuickItem> m_image;  // REAL QtQuick Image (source + texture provider)
    QPointer<QQuickItem> m_effect; // REAL QtQuick.Effects MultiEffect (visible renderer)
};
