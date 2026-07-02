#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

class CssTheme;

// Non-visual CSS enabler, translated 1:1 from qml/qmlcss/CssItem.qml — BY COMPOSITION. Drop a
// CssItem into ANY QtQuick builtin to make it CSS-styled — the styling analogue of MouseArea: a
// non-visual child (visible:false, 0×0) that carries the CssQmlItem identity signature, registers
// itself with cssTheme.loadCss, and applies the resolved rules imperatively onto its parentItem().
//
// The parent's type is inferred from its properties:
//   text + font  → "text"  — applies color, font-family, font-size
//   radius + color → "rect" — applies background-color, border-radius, border-color/width
//   else          → "item"  — nothing applied (no mappable visual props)
//
// The engine re-pushes on theme reload and cssId/cssClass change (the reverse slot), so the
// imperative apply never goes stale.
//
// Example:
//   Text { text: "hi"; CssItem { cssId: "foo" } }
class CssItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    // What the parent is, so the right CSS→QML mapping is used. Inferred from the parent when empty.
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    // Engine writes the resolved rules here; apply() pushes them onto the parent.
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)

public:
    explicit CssItem(QQuickItem *parent = nullptr);

    QString cssId() const { return m_cssId; }
    void setCssId(const QString &v);

    QVariant cssAlternateId() const { return m_cssAlternateId; }
    void setCssAlternateId(const QVariant &v);

    QVariant cssClass() const { return m_cssClass; }
    void setCssClass(const QVariant &v);

    QString cssPrimitive() const { return m_cssPrimitive; }
    void setCssPrimitive(const QString &v);

    QString cssPart() const { return m_cssPart; }
    void setCssPart(const QString &v);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    // QML hasCssIdentity(): cssId/cssPart non-empty, or cssClass non-empty list.
    bool hasCssIdentity() const;

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void styleChanged();

protected:
    void componentComplete() override;

private:
    // Infer parent primitive type from its properties (QML _inferPrimitive()).
    QString inferPrimitive() const;
    // Push resolved style rules onto parentItem() (QML _apply()).
    void apply();
    // Re-style via the engine's reverse slot when identity changes (QML onCssId/ClassChanged).
    void maybeLoadCss();

    QString  m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass       = QVariant::fromValue(QVariantList());
    QString  m_cssPrimitive;
    QString  m_cssPart;
    QVariantMap m_style;

    QPointer<CssTheme> m_theme;
};
