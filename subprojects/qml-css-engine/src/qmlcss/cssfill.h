#pragma once

#include <QColor>
#include <QPointer>
#include <QQmlListProperty>
#include <QQuickItem>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

class CssTheme;
class CssLayoutEngine;

// Thin compatibility shim over CssRect, translated 1:1 from qml/qmlcss/CssFill.qml — BY
// COMPOSITION, not reimplementation. A Shape cannot fill with an image, so CssFill adds a
// `background-image` layer BEHIND a REAL CssRect; everything else — solid colour, gradient,
// border, box-shadow, inset bevel and the CSS `transition` fade — is delegated to that CssRect.
// New code should use CssRect directly; CssFill remains for container/popup callers that use
// url() image backgrounds.
//
// It composes (bottom -> top):
//   * a solid-colour background bar (a REAL QtQuick Shape/PathRectangle — Rectangle is avoided
//     per the owner's rule) shown only when a `background-image` is present, so an image with
//     alpha (or one still loading) sits on the resolved `background-color`;
//   * a REAL QtQuick Image (via the Qt type-system) for the url() background, with object-fit /
//     alignment / opacity from the style;
//   * a REAL CssRect (our own registered type — cssPrimitive "" so the theme registry never
//     overwrites its explicit `style`) that renders the fill/border/gradient/shadow/bevel/
//     transition; when an image is present its fill is forced transparent so the image shows
//     through and the border still frames it;
//   * an inner `contentHolder` hosting declared children (the QML `default property alias
//     content: contentHolder.data`), laid out through cssLayout exactly like CssRect.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/
// cssPart/style) plus the renderer defaults (radius/defaultColor/…) it forwards to the CssRect,
// and the inherited text properties (CSS inheritance) that CssText children read.
class CssFill : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)

    // Renderer defaults forwarded to the composed CssRect (the QML `default*` props).
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(QColor defaultColor READ defaultColor WRITE setDefaultColor NOTIFY defaultColorChanged)
    Q_PROPERTY(QColor defaultBorderColor READ defaultBorderColor WRITE setDefaultBorderColor NOTIFY defaultBorderColorChanged)
    Q_PROPERTY(qreal defaultBorderWidth READ defaultBorderWidth WRITE setDefaultBorderWidth NOTIFY defaultBorderWidthChanged)

    // Deprecated no-ops: the transition is derived from `style` by CssRect now. Kept so existing
    // callers that still assign these don't error — they have no effect.
    Q_PROPERTY(int transitionMs READ transitionMs WRITE setTransitionMs NOTIFY transitionMsChanged)
    Q_PROPERTY(int transitionEasingType READ transitionEasingType WRITE setTransitionEasingType NOTIFY transitionEasingTypeChanged)

    // Inherited text properties (CSS inheritance) — own style wins, else the CSS-inheriting
    // ancestor (the containing box at parent.parent). CssText children read these.
    Q_PROPERTY(QString inheritedColor READ inheritedColor NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontFamily READ inheritedFontFamily NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontSize READ inheritedFontSize NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontWeight READ inheritedFontWeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLineHeight READ inheritedLineHeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLetterSpacing READ inheritedLetterSpacing NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextTransform READ inheritedTextTransform NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextAlign READ inheritedTextAlign NOTIFY inheritedChanged)

    // The container's default child slot (QML `default property alias content: contentHolder.data`).
    Q_PROPERTY(QQmlListProperty<QObject> content READ content)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    explicit CssFill(QQuickItem *parent = nullptr);

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

    qreal radius() const { return m_radius; }
    void setRadius(qreal v);
    QColor defaultColor() const { return m_defaultColor; }
    void setDefaultColor(const QColor &v);
    QColor defaultBorderColor() const { return m_defaultBorderColor; }
    void setDefaultBorderColor(const QColor &v);
    qreal defaultBorderWidth() const { return m_defaultBorderWidth; }
    void setDefaultBorderWidth(qreal v);

    int transitionMs() const { return m_transitionMs; }
    void setTransitionMs(int v);
    int transitionEasingType() const { return m_transitionEasingType; }
    void setTransitionEasingType(int v);

    QString inheritedColor() const;
    QString inheritedFontFamily() const;
    QString inheritedFontSize() const;
    QString inheritedFontWeight() const;
    QString inheritedLineHeight() const;
    QString inheritedLetterSpacing() const;
    QString inheritedTextTransform() const;
    QString inheritedTextAlign() const;

    QQmlListProperty<QObject> content();

    // QML `requestRelayout()` — invoked by CssLayoutEngine::notifyParentLayout and on our own
    // geometry/style/children changes. Must be invokable (invokeMethod by name).
    Q_INVOKABLE void requestRelayout();

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssStateChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void styleChanged();
    void hasCssIdentityChanged();
    void radiusChanged();
    void defaultColorChanged();
    void defaultBorderColorChanged();
    void defaultBorderWidthChanged();
    void transitionMsChanged();
    void transitionEasingTypeChanged();
    void inheritedChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Re-run the CSS->value resolution the QML `readonly property` bindings did (solid colour,
    // image source, fill mode, image opacity) and push it onto the composed background Shape /
    // Image, and forward `style`/defaults onto the composed CssRect renderer.
    void recompute();
    // anchors.fill equivalent: keep every composed layer + content holder sized to us.
    void layoutChildren();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss();

    QString m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass = QVariant::fromValue(QVariantList());
    QVariant m_cssState = QVariant::fromValue(QVariantList());
    QString m_cssPrimitive = QStringLiteral("rect");
    QString m_cssPart;
    QVariantMap m_style;

    qreal m_radius = 0;
    QColor m_defaultColor = QColor(Qt::transparent);
    QColor m_defaultBorderColor = QColor(Qt::transparent);
    qreal m_defaultBorderWidth = 0;
    int m_transitionMs = 0;
    int m_transitionEasingType = 0;

    QPointer<CssTheme> m_theme;
    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_bgSolid;       // REAL QtQuick Shape solid background (behind the image)
    QPointer<QQuickItem> m_image;         // REAL QtQuick Image (the url() background)
    QPointer<QQuickItem> m_rect;          // REAL CssRect renderer (fill/border/gradient/shadow)
    QPointer<QQuickItem> m_contentHolder; // hosts declared children (the layout participants)
};
