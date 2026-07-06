// MonthGrid — the shared month-grid calendar body in solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component). Used by both Calendar (inline) and DateField (in its popup). A plain Item
// hosting the header (prev/next `.cal-nav` buttons + `.cal-title`), the DayOfWeekRow and the
// AbstractMonthGrid with its day delegate. The Abstract templates instantiate NO delegates in C++ —
// the style (us) supplies contentItems whose Repeaters bind control.source → control.delegate.
//
// Props:
//   selectedDate — the currently-selected Date (drives the `.day:selected` highlight)
//   cursorDate   — the keyboard cursor Date (drives `.day:focus`); null for the inline Calendar
//   viewMonth/viewYear — the shown month/year; initialised from selectedDate (the binding breaks on
//     the first nav click — QML semantics — after which they track navigation). DateField writes them
//     imperatively from its keyboard stepping, so nav and keyboard share the same state.
//   signal dayPicked(date) — fires new Date(year, month, day) when a day cell is clicked.
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Item {
    id: root
    property var selectedDate: null
    property var cursorDate: null
    property int viewMonth: selectedDate instanceof Date ? selectedDate.getMonth() : new Date().getMonth()
    property int viewYear: selectedDate instanceof Date ? selectedDate.getFullYear() : new Date().getFullYear()
    signal dayPicked(var date)

    // Fixed implicit size: 7 cells × 32 px wide; 32 (header) + 24 (DOW) + 6 rows × 32 = 280.
    implicitWidth: 224
    implicitHeight: 280

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
