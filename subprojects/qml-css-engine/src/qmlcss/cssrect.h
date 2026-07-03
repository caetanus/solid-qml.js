#pragma once

#include <QColor>
#include <QPointer>
#include <QQmlListProperty>
#include <QQuickItem>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

class CssTheme;
class QVariantAnimation;
class CssLayoutEngine;

// Native CSS box, translated 1:1 from qml/qmlcss/CssRect.qml — BY COMPOSITION, not
// reimplementation. A CssRect is BOTH a painted CSS box AND a layout container for its content.
//
// It does NOT paint anything itself: it instantiates a REAL QtQuick render subtree via the Qt
// type-system (a `Shape` fill — a closed rounded-rect ShapePath, so solid colour, linear/radial
// gradients, per-corner border-radius and the alpha-fix all fall out of the Shape's fill; a
// separate border `Shape`; per-side border + inset-bevel lines as `Shape`/`PathRectangle` bars;
// `box-shadow` via a REAL QtQuick.Effects `MultiEffect`) and drives that subtree's inputs from
// C++ (the CSS→value resolution the QML `readonly property` bindings did, via cssTheme). Rectangle
// is deliberately avoided per the owner's rule (it is capped for several effects); Shape is the
// general vector path.
//
// The fill's alpha rides on the fill Shape's `opacity` (a translucent Shape fill blends wrong —
// render opaque, alpha via opacity), and the render subtree is a SIBLING of the content holder,
// so that opacity dims only the fill, never the element's content.
//
// As a container it hosts declared children in an inner `contentHolder` (the QML
// `default property alias content: contentHolder.data`) and drives cssLayout.requestLayout(...)
// on geometry/style/children changes — the box model (flex/grid/calc/…) runs in C++
// (CssLayoutEngine); this only TRIGGERS a relayout.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/
// cssPart/style); resolved rules are pushed back into `style` by cssTheme.loadCss(this).
class CssRect : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)

    // Engine-driven hover: the theme flips cssHoverStyled when an applicable `:hover` rule
    // exists (we then compose a HoverHandler); the handler writes cssEngineHover, whose
    // notify re-applies the style with the extra "hover" state class.
    Q_PROPERTY(bool cssHoverStyled READ cssHoverStyled WRITE setCssHoverStyled NOTIFY cssHoverStyledChanged)
    Q_PROPERTY(bool cssEngineHover READ cssEngineHover WRITE setCssEngineHover NOTIFY cssStateChanged)

public:
    // --- CSS transitions (item-level) ----------------------------------------------------
    // The declared `transition` (property/duration/easing), parsed at style apply. The layout
    // engine consults this when writing width/height, so declared geometry transitions
    // animate instead of snapping. Covers-check: empty/`all` or the exact property.
    bool transitionCovers(QLatin1String prop) const
    {
        return m_transMs > 0
            && (m_transProp.isEmpty() || m_transProp == QLatin1String("all") || m_transProp == prop);
    }
    // Animate width/height to `target`; returns false when no covering transition (the caller
    // writes directly). Idempotent while already animating toward the same target.
    bool animateGeometry(QLatin1String prop, qreal target);

private:

    // Renderer defaults (used when the style omits the longhand) — the QML `default*` props,
    // set by the CssFill shim.
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(QColor defaultColor READ defaultColor WRITE setDefaultColor NOTIFY defaultColorChanged)
    Q_PROPERTY(QColor defaultBorderColor READ defaultBorderColor WRITE setDefaultBorderColor NOTIFY defaultBorderColorChanged)
    Q_PROPERTY(qreal defaultBorderWidth READ defaultBorderWidth WRITE setDefaultBorderWidth NOTIFY defaultBorderWidthChanged)

    // Inherited text properties (CSS inheritance) — own style wins, else the CSS-inheriting
    // ancestor (the containing CssRect at parent.parent). CssText children read these.
    Q_PROPERTY(QString inheritedColor READ inheritedColor NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontFamily READ inheritedFontFamily NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontSize READ inheritedFontSize NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontWeight READ inheritedFontWeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLineHeight READ inheritedLineHeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLetterSpacing READ inheritedLetterSpacing NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextTransform READ inheritedTextTransform NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextAlign READ inheritedTextAlign NOTIFY inheritedChanged)

    // Written by CssLayoutEngine::applyAnim (transform keyframe interpolation); drive the item's
    // rotation/scale/translate while an `animation` runs.
    Q_PROPERTY(qreal _animRotate READ animRotate WRITE setAnimRotate NOTIFY animChanged)
    Q_PROPERTY(qreal _animScale READ animScale WRITE setAnimScale NOTIFY animChanged)
    Q_PROPERTY(qreal _animTx READ animTx WRITE setAnimTx NOTIFY animChanged)
    Q_PROPERTY(qreal _animTy READ animTy WRITE setAnimTy NOTIFY animChanged)

    // Normalised animation position 0→1; driven by the composed NumberAnimation (see
    // componentComplete). On every tick, the setter calls cssLayout.applyAnim() to interpolate
    // the @keyframes stops and write _animRotate/_animScale/_animTx/_animTy. Equivalent to the
    // QML `property real animTick` + `onAnimTickChanged` + `NumberAnimation on animTick`.
    Q_PROPERTY(qreal animTick READ animTick WRITE setAnimTick NOTIFY animTickChanged)

    // The container's default child slot (QML `default property alias content: contentHolder.data`).
    Q_PROPERTY(QQmlListProperty<QObject> content READ content)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    explicit CssRect(QQuickItem *parent = nullptr);

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

    bool cssHoverStyled() const { return m_hoverStyled; }
    void setCssHoverStyled(bool v);
    bool cssEngineHover() const { return m_engineHover; }
    void setCssEngineHover(bool v);
    Q_SLOT void onEngineHoverChanged(); // composed HoverHandler.hoveredChanged -> cssEngineHover
    // Compose the Flickable and move the content holder into it (overflow-y: auto/scroll).
    void ensureScrollable();
    Q_SLOT void syncScrollContent();

    bool hasCssIdentity() const;

    qreal radius() const { return m_radius; }
    void setRadius(qreal v);
    QColor defaultColor() const { return m_defaultColor; }
    void setDefaultColor(const QColor &v);
    QColor defaultBorderColor() const { return m_defaultBorderColor; }
    void setDefaultBorderColor(const QColor &v);
    qreal defaultBorderWidth() const { return m_defaultBorderWidth; }
    void setDefaultBorderWidth(qreal v);

    QString inheritedColor() const;
    QString inheritedFontFamily() const;
    QString inheritedFontSize() const;
    QString inheritedFontWeight() const;
    QString inheritedLineHeight() const;
    QString inheritedLetterSpacing() const;
    QString inheritedTextTransform() const;
    QString inheritedTextAlign() const;

    qreal animRotate() const { return m_animRotate; }
    void setAnimRotate(qreal v);
    qreal animScale() const { return m_animScale; }
    void setAnimScale(qreal v);
    qreal animTx() const { return m_animTx; }
    void setAnimTx(qreal v);
    qreal animTy() const { return m_animTy; }
    void setAnimTy(qreal v);

    qreal animTick() const { return m_animTick; }
    void setAnimTick(qreal v);

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
    void cssHoverStyledChanged();
    void radiusChanged();
    void defaultColorChanged();
    void defaultBorderColorChanged();
    void defaultBorderWidthChanged();
    void inheritedChanged();
    void animChanged();
    void animTickChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Re-run the CSS→value resolution the QML `readonly property` bindings did (colours, radii,
    // gradient, border, box-shadow, transform, transition) and push it onto the composed render
    // subtree. The equivalent of every `style`-derived binding in CssRect.qml.
    void recompute();
    // anchors.fill equivalent: keep the render subtree + content holder sized to us.
    void layoutChildren();
    // Applies the active transform — if _animActive (stops.length >= 2), uses the animated
    // _animRotate/_animScale/_animTx/_animTy (written each tick by applyAnim); otherwise uses the
    // static transform resolved from style["transform"]. Pivot is the element centre (CSS default,
    // QML: transformOrigin: Item.Center). Faithful to the QML rotation/scale/transform bindings.
    void applyTransform();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss();
    // The CSS-inheriting ancestor (containing CssRect at parent.parent), or null.
    CssRect *cssParent() const;

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

    // Animated transform channels — written each tick by CssLayoutEngine::applyAnim().
    qreal m_animRotate = 0;
    qreal m_animScale = 1;
    qreal m_animTx = 0;
    qreal m_animTy = 0;

    // Static (non-animated) transform resolved from `style` (rotate deg / scale / tx,ty px).
    qreal m_staticRotate = 0;
    qreal m_staticScale = 1;
    qreal m_staticTx = 0;
    qreal m_staticTy = 0;

    // @keyframes animation driver — QML-equivalent:
    //   property real animTick: 0
    //   NumberAnimation on animTick { from: 0.0; to: 1.0; duration: ...; loops: ...; easing: ... }
    //   onAnimTickChanged: if (cssLayout) cssLayout.applyAnim(root, _animStops, animTick)
    qreal m_animTick = 0.0;
    bool m_animActive = false;           // _animStops.length >= 2

    // --- CSS transition state (parsed from `transition` at style apply) -------------------
    int m_transMs = 0;
    int m_transEasing = 1; // QEasingCurve::InOutQuad set at parse; int to avoid the header dep
    QString m_transProp;
    QVariantAnimation *m_opacityAnim = nullptr; // lazy; animates our own opacity
    QVariantAnimation *m_widthAnim = nullptr;   // lazy; driven via animateGeometry
    QVariantAnimation *m_heightAnim = nullptr;
    qreal m_widthAnimTarget = -1;
    qreal m_heightAnimTarget = -1;

    // overflow(-y): auto/scroll — composed Flickable hosting the contentHolder.
    QPointer<QQuickItem> m_flickable;

    // Engine-driven hover tracking (see cssHoverStyled/cssEngineHover above).
    bool m_hoverStyled = false;
    bool m_engineHover = false;
    QPointer<QObject> m_hoverHandler; // composed QtQuick HoverHandler (created on demand)
    QVariantList m_animStops;            // buildAnimStops output for current @keyframes
    QPointer<QObject> m_anim;            // REAL QtQuick NumberAnimation on animTick (0→1)

    QPointer<CssTheme> m_theme;
    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_render;       // the REAL QtQuick render subtree (Shapes + MultiEffect)
    QPointer<QQuickItem> m_contentHolder; // hosts declared children (the layout participants)
    QPointer<QObject> m_translate;        // the QtQuick Translate in our transform list
};
