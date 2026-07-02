#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>

class CssTheme;
class CssLayoutEngine;

// Native CSS-styled label, translated 1:1 from qml/qmlcss/CssText.qml — BY COMPOSITION, not
// reimplementation. Unlike the QML (whose ROOT *is* a Text), this C++ CssText is a QQuickItem that
// composes a REAL QtQuick `Text` via the Qt type-system and FORWARDS the content plus every
// CSS-driven property onto it. It paints nothing itself; the composed Text does.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/cssPart/
// style); resolved rules are pushed back into `style` by cssTheme.loadCss(this), and the forwarded
// Text props key off it.
//
// CSS inheritance mirrors CssRect/CssFill: inheritable text props come from this label's own style,
// else from the CSS-inheriting ancestor. The chain walks `parent` (a directly-styled parent whose
// inheritedColor is non-empty) then `parent.parent` (the containing box), reading the inherited*
// getters off whichever ancestor exposes them — so a bare label picks up the container's colour/font.
//
// Forwarded onto the composed Text:
//   text; color<-inheritedColor; font.family/pointSize/weight/letterSpacing/wordSpacing<-inherited*;
//   font.underline/strikeout<-text-decoration; font.capitalization<-text-transform; wrapMode<-white-
//   space; elide<-text-overflow; horizontalAlignment<-text-align; verticalAlignment; textFormat
//   PlainText; lineHeight(Mode)<-line-height; padding<-padding; visible<-display:none;
//   opacity<-visibility/opacity. font.family re-resolves on cssTheme.fontRevision (async @font-face).
//
// text-shadow is composed as a REAL QtQuick.Effects MultiEffect drop-shadow (same idiom as
// the C++ CssDropShadow) sourcing the Text — mirrors the QML `layer.effect: CssDropShadow{}`
// pattern (CssDropShadow is now also a C++ type; CssText inlines the equivalent directly).
//
// Layout participant: implicitWidth/Height mirror the composed Text's; notifyParentLayout on
// implicit/visible/style change (like CssHr).
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   Item {                                    // <- this CssText
//       Text {                                // <- m_text, fills the item
//           text: root.text
//           color: ...; font: ...; wrapMode: ...; elide: ...; textFormat: Text.PlainText
//           horizontalAlignment: ...; verticalAlignment: Text.AlignVCenter; ...padding
//           visible: !(display none); opacity: visibility/opacity
//       }
//       MultiEffect { source: text; shadow* ... }   // <- m_shadow, only when text-shadow present
//   }
class CssText : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)

    // The label content (QML: property alias text: root.text — the Text's own text).
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)

    // Fallbacks when the theme leaves the label unstyled (the QML default* props).
    Q_PROPERTY(QColor defaultColor READ defaultColor WRITE setDefaultColor NOTIFY defaultColorChanged)
    Q_PROPERTY(QString defaultFontFamily READ defaultFontFamily WRITE setDefaultFontFamily NOTIFY defaultFontFamilyChanged)
    Q_PROPERTY(qreal defaultFontSize READ defaultFontSize WRITE setDefaultFontSize NOTIFY defaultFontSizeChanged)

    // Inherited text properties (CSS inheritance) — own style wins, else the CSS-inheriting ancestor.
    Q_PROPERTY(QString inheritedColor READ inheritedColor NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontFamily READ inheritedFontFamily NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontSize READ inheritedFontSize NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontWeight READ inheritedFontWeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLineHeight READ inheritedLineHeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLetterSpacing READ inheritedLetterSpacing NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextTransform READ inheritedTextTransform NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextAlign READ inheritedTextAlign NOTIFY inheritedChanged)

public:
    explicit CssText(QQuickItem *parent = nullptr);

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

    QString text() const { return m_text; }
    void setText(const QString &v);

    QColor defaultColor() const { return m_defaultColor; }
    void setDefaultColor(const QColor &v);
    QString defaultFontFamily() const { return m_defaultFontFamily; }
    void setDefaultFontFamily(const QString &v);
    qreal defaultFontSize() const { return m_defaultFontSize; }
    void setDefaultFontSize(qreal v);

    QString inheritedColor() const;
    QString inheritedFontFamily() const;
    QString inheritedFontSize() const;
    QString inheritedFontWeight() const;
    QString inheritedLineHeight() const;
    QString inheritedLetterSpacing() const;
    QString inheritedTextTransform() const;
    QString inheritedTextAlign() const;

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssStateChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void styleChanged();
    void hasCssIdentityChanged();
    void textChanged();
    void defaultColorChanged();
    void defaultFontFamilyChanged();
    void defaultFontSizeChanged();
    void inheritedChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private slots:
    // An ancestor's inherited text props changed (CSS inheritance): re-apply + re-propagate.
    void onAncestorInheritedChanged();

private:
    // Resolve every CSS-driven value (the QML `readonly property`/binding set) and push it onto the
    // composed Text — the C++ equivalent of all of CssText.qml's Text bindings.
    void applyToText();
    // Recompute + push the text-shadow drop-shadow onto the composed MultiEffect.
    void applyShadow();
    // Keep the composed Text (and shadow) sized to us; when unmanaged (no assigned size), let the
    // Text use its own implicit size so it never collapses.
    void layoutChild();
    // Mirror the composed Text's implicit size onto us (we are the layout participant).
    void mirrorImplicit();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss();
    // The CSS-inheriting ancestor (QML `_cssParent`): a directly-styled parent, else parent.parent.
    QQuickItem *cssParentItem() const;
    // Own style value, else the ancestor's same inherited getter (the QML inherited* chain).
    QString inheritedValue(const char *styleKey, const char *getterName) const;

    QString m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass = QVariant::fromValue(QVariantList());
    QVariant m_cssState = QVariant::fromValue(QVariantList());
    QString m_cssPrimitive = QStringLiteral("text");
    QString m_cssPart;
    QVariantMap m_style;
    QString m_text;

    QColor m_defaultColor = QColor(Qt::black);
    QString m_defaultFontFamily = QStringLiteral("Sans Serif");
    qreal m_defaultFontSize = 11;

    QPointer<CssTheme> m_theme;
    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_label; // the REAL QtQuick Text, via the type-system
    QPointer<QQuickItem> m_shadow; // the REAL QtQuick.Effects MultiEffect drop-shadow (text-shadow)
};
