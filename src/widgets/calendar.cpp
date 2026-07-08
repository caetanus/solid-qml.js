#include "calendar.h"

#include "snippetwidget.h"

namespace {

// Original Calendar.qml internals — `root` = the C++ wrapper. The anchored plain Item insulates
// the grid from the outer CSS layout engine (only Css.* direct children are layout participants).
const char *kCalendarBody = R"(import QtQuick
import solidqml.Widgets 1.0 as W

Item {
    anchors.fill: parent
    W.MonthGrid {
        anchors.fill: parent
        selectedDate: root.value
        onDayPicked: (date) => root.dayPicked(date)
    }
}
)";

} // namespace

namespace SolidWidgets {

Calendar::Calendar(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
    // Fixed implicit size: 7 cells × 32 px wide; 32 (header) + 24 (DOW) + 6 rows × 32 = 280.
    setImplicitWidth(224);
    setImplicitHeight(280);
}

void Calendar::setValue(const QVariant &v)
{
    if (m_value == v)
        return;
    m_value = v;
    emit valueChanged();
}

void Calendar::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    // Into the CONTENT slot (the padding box), like the declared child was — the anchored plain
    // Item is skipped by the flex pass but keeps the grid inside the padding box.
    composeInternal(this, content(), QStringLiteral("solidwidgets-calendar"), kCalendarBody);
}

} // namespace SolidWidgets
