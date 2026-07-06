// Calendar — the <Calendar> inline month-grid widget in solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component). Wrapper CssFill (cssPrimitive "div") — participates in the parent's
// flex/grid layout — hosting a MonthGrid inside a plain Item that insulates the grid from the outer
// CSS layout engine (only Css.* direct children are layout participants).
//
// `value` (the selected Date) drives the :selected highlight; picking a day relays via `dayPicked`
// so the emit's onChange handler fires.
//
// The transpiler emits:  W.Calendar { cssClass: […]; [value: <expr>]; [onDayPicked: (date) => {…}] }
import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property var value: null
    signal dayPicked(var date)

    cssPrimitive: "div"
    // Fixed implicit size: 7 cells × 32 px wide; 32 (header) + 24 (DOW) + 6 rows × 32 = 280.
    implicitWidth: 224
    implicitHeight: 280

    Item {
        anchors.fill: parent
        MonthGrid {
            anchors.fill: parent
            selectedDate: root.value
            onDayPicked: (date) => root.dayPicked(date)
        }
    }
}
