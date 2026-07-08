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

public:
    explicit Select(QQuickItem *parent = nullptr);

    QVariant model() const { return m_model; }
    void setModel(const QVariant &v);
    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);
    QVariant values() const { return m_values; }
    void setValues(const QVariant &v);

signals:
    void modelChanged();
    void currentIndexChanged();
    void valuesChanged();
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
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
