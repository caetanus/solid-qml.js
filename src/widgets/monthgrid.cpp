#include "monthgrid.h"

#include "snippetwidget.h"

namespace {

// Original MonthGrid.qml internals — `root` = the C++ wrapper; an anchored Item hosts the four
// absolutely-positioned children so the snippet has a single root. The Abstract templates
// instantiate NO delegates in C++ — the style (us) supplies contentItems whose Repeaters bind
// control.source → control.delegate.
const char *kMonthGridBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Item {
    anchors.fill: parent

    // ── prev nav button ────────────────────────────────────────────────────
    Css.CssFill {
        cssPrimitive: "button"
        cssClass: ["cal-nav"]
        x: 0
        y: 0
        width: 32
        height: 32
        implicitWidth: 32
        implicitHeight: 32
        Css.CssText { cssPrimitive: ""; text: "‹"; anchors.centerIn: parent }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.viewMonth === 0) { root.viewYear = root.viewYear - 1; root.viewMonth = 11 }
                else root.viewMonth = root.viewMonth - 1
            }
        }
    }
    // ── month/year label ───────────────────────────────────────────────────
    Css.CssText {
        cssPrimitive: ""
        cssClass: ["cal-title"]
        x: 32
        y: 0
        width: parent.width - 64
        height: 32
        text: Qt.locale().monthName(root.viewMonth) + " " + root.viewYear
        // CssText does not expose horizontalAlignment as a QML property; verticalAlignment is always
        // AlignVCenter inside CssText; horizontal centering is driven by the style key "text-align".
        style: ({"text-align": "center"})
    }
    // ── next nav button ────────────────────────────────────────────────────
    Css.CssFill {
        cssPrimitive: "button"
        cssClass: ["cal-nav"]
        x: parent.width - 32
        y: 0
        width: 32
        height: 32
        implicitWidth: 32
        implicitHeight: 32
        Css.CssText { cssPrimitive: ""; text: "›"; anchors.centerIn: parent }
        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.viewMonth === 11) { root.viewYear = root.viewYear + 1; root.viewMonth = 0 }
                else root.viewMonth = root.viewMonth + 1
            }
        }
    }
    // ── day-of-week row ────────────────────────────────────────────────────
    T.AbstractDayOfWeekRow {
        id: dow
        x: 0
        y: 32
        width: parent.width
        height: 24
        contentItem: Row {
            Repeater {
                model: dow.source
                delegate: dow.delegate
            }
        }
        // Delegate host is a plain Item sized DECLARATIVELY with the template's own cell formula. The
        // C++ resizeItems() only fires on geometryChange — under CssIncubator the Repeater populates
        // after the last geometry change, so imperative sizing never lands and the Row stacks 0-wide
        // cells. A binding is timing-proof. The CssText centres inside the cell.
        delegate: Item {
            width: (dow.contentItem.width - 6 * dow.spacing) / 7
            height: dow.contentItem.height
            Css.CssText {
                cssPrimitive: ""
                cssClass: ["dow"]
                text: model.shortName
                anchors.centerIn: parent
            }
        }
    }
    // ── month grid ─────────────────────────────────────────────────────────
    T.AbstractMonthGrid {
        id: mg
        x: 0
        y: 56
        width: parent.width
        height: parent.height - 56
        month: root.viewMonth
        year: root.viewYear
        contentItem: Grid {
            columns: 7
            rows: 6
            Repeater {
                model: mg.source
                delegate: mg.delegate
            }
        }
        delegate: T.AbstractButton {
            id: mgDel
            implicitWidth: 32
            implicitHeight: 32
            // Declarative cell size (same formula as the template's resizeItems) — see the
            // day-of-week delegate note: incubated creation misses the imperative resize.
            width: (mg.contentItem.width - 6 * mg.spacing) / 7
            height: (mg.contentItem.height - 5 * mg.spacing) / 6
            // Shared day pseudo-class state. cursorDate is null for the inline Calendar → the focus
            // concat never matches. background and contentItem are SIBLING slots, so they both carry
            // the state (`.day:outside .day-label` can never match; `.day-label:outside` does).
            readonly property var __dayState: (model.today ? ["today"] : []).concat((root.selectedDate instanceof Date && model.year === root.selectedDate.getFullYear() && model.month === root.selectedDate.getMonth() && model.day === root.selectedDate.getDate()) ? ["selected"] : []).concat(model.month !== mg.month ? ["outside"] : []).concat(mgDel.hovered ? ["hover"] : []).concat((root.cursorDate instanceof Date && model.year === root.cursorDate.getFullYear() && model.month === root.cursorDate.getMonth() && model.day === root.cursorDate.getDate()) ? ["focus"] : [])
            background: Css.CssFill {
                cssPrimitive: "div"
                cssClass: ["day"]
                cssState: mgDel.__dayState
            }
            contentItem: Css.CssText {
                cssPrimitive: ""
                cssClass: ["day-label"]
                cssState: mgDel.__dayState
                text: model.day
            }
            onClicked: root.dayPicked(new Date(model.year, model.month, model.day))
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

MonthGrid::MonthGrid(QQuickItem *parent)
    : QQuickItem(parent)
{
    // Fixed implicit size: 7 cells × 32 px wide; 32 (header) + 24 (DOW) + 6 rows × 32 = 280.
    setImplicitWidth(224);
    setImplicitHeight(280);
}

QDate MonthGrid::baseDate() const
{
    const QDateTime dt = m_selectedDate.toDateTime();
    return dt.isValid() ? dt.date() : QDate::currentDate();
}

void MonthGrid::setSelectedDate(const QVariant &v)
{
    if (m_selectedDate == v)
        return;
    m_selectedDate = v;
    emit selectedDateChanged();
    // Unwritten view props follow the selected date (JS init-binding semantics).
    if (!m_viewMonthSet)
        emit viewMonthChanged();
    if (!m_viewYearSet)
        emit viewYearChanged();
}

void MonthGrid::setCursorDate(const QVariant &v)
{
    if (m_cursorDate == v)
        return;
    m_cursorDate = v;
    emit cursorDateChanged();
}

int MonthGrid::viewMonth() const
{
    if (m_viewMonthSet)
        return m_viewMonth;
    return baseDate().month() - 1; // JS Date months are 0-based
}

void MonthGrid::setViewMonth(int v)
{
    const bool same = m_viewMonthSet && m_viewMonth == v;
    m_viewMonth = v;
    m_viewMonthSet = true; // first write breaks the follow (QML binding-break semantics)
    if (!same)
        emit viewMonthChanged();
}

int MonthGrid::viewYear() const
{
    if (m_viewYearSet)
        return m_viewYear;
    return baseDate().year();
}

void MonthGrid::setViewYear(int v)
{
    const bool same = m_viewYearSet && m_viewYear == v;
    m_viewYear = v;
    m_viewYearSet = true;
    if (!same)
        emit viewYearChanged();
}

void MonthGrid::componentComplete()
{
    QQuickItem::componentComplete();
    composeInternalPlain(this, QStringLiteral("solidwidgets-monthgrid"), kMonthGridBody);
}

} // namespace SolidWidgets
