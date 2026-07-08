#pragma once

#include "qmlcss/cssfill.h"

#include <QVariant>

// Calendar (port of Calendar.qml) — the inline month grid. Wrapper CssFill (cssPrimitive "div")
// participates in the parent's flex/grid layout; the snippet hosts a W.MonthGrid inside a plain
// Item that insulates the grid from the outer CSS layout engine. `value` (the selected Date)
// drives the :selected highlight; picking a day relays via dayPicked (→ the emit's onChange).
namespace SolidWidgets {

class Calendar : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QVariant value READ value WRITE setValue NOTIFY valueChanged)

public:
    explicit Calendar(QQuickItem *parent = nullptr);

    QVariant value() const { return m_value; }
    void setValue(const QVariant &v);

signals:
    void valueChanged();
    void dayPicked(const QVariant &date);

protected:
    void componentComplete() override;

private:
    QVariant m_value;
};

} // namespace SolidWidgets
