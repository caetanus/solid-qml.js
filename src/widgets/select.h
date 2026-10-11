#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QVariant>

// Select (port of Select.qml) — the <select>/<option> T.ComboBox. The C++ wrapper owns the
// controlled `currentIndex` (RestoreNone Binding in, onCurrentIndexChanged mirror out), the
// display-label `model`, the parallel `values` array and the focus/disabled cssState; the
// T.ComboBox with the `.value` contentItem, `.chevron` glyph, `.option` delegates and the
// window-bounds-flipping in-scene popup (cssAncestor re-anchored on both sibling slots) rides
// the original QML body as a `root`-bound snippet. User picks relay via activated(index).
namespace SolidWidgets {

class Select : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QVariant values READ values WRITE setValues NOTIFY valuesChanged)
    // Parallel to `values`/`model`: one bool per row, true when that option should not be pickable.
    // Defaults to an empty list — `[][index]` reads undefined/false, so a <select> that never sets
    // this (every one before this plan) behaves exactly as before.
    Q_PROPERTY(QVariant disabledFlags READ disabledFlags WRITE setDisabledFlags NOTIFY disabledFlagsChanged)
    // aria-label/aria-description (release-plan M5 follow-up): routed to the inner T.ComboBox —
    // the item that actually takes focus. A ComboBox's default accessible name comes from its
    // current text, which an author may want to override (e.g. an icon-only select).
    Q_PROPERTY(QString accessibleName READ accessibleName WRITE setAccessibleName NOTIFY accessibleNameChanged)
    Q_PROPERTY(QString accessibleDescription READ accessibleDescription WRITE setAccessibleDescription NOTIFY accessibleDescriptionChanged)

public:
    explicit Select(QQuickItem *parent = nullptr);

    QVariant model() const { return m_model; }
    void setModel(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);
    QVariant values() const { return m_values; }
    void setValues(const QVariant &v);
    QVariant disabledFlags() const { return m_disabledFlags; }
    void setDisabledFlags(const QVariant &v);
    QString accessibleName() const { return m_accessibleName; }
    void setAccessibleName(const QString &v);
    QString accessibleDescription() const { return m_accessibleDescription; }
    void setAccessibleDescription(const QString &v);

signals:
    void modelChanged();
    void currentIndexChanged();
    void valuesChanged();
    void disabledFlagsChanged();
    void accessibleNameChanged();
    void accessibleDescriptionChanged();
    void activated(int index);

protected:
    void componentComplete() override;

private:
    Q_SLOT void syncState();

    QVariant m_model;
    // 0, not ComboBox's empty-model -1: the snippet's RestoreNone Binding pushes this value, so
    // -1 would override the ComboBox's select-first-row-when-the-model-lands and blank the box.
    int m_currentIndex = 0;
    QVariant m_values = QVariantList();
    QVariant m_disabledFlags = QVariantList();
    QString m_accessibleName;
    QString m_accessibleDescription;
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
