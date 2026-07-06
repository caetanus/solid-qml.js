// Chart — a 2D chart in module solidqml.Widgets (owner directive 2026-07-06: "use CssFill for
// QtQuick3D e QtGraphs"). A CssFill "div" wrapper (so the chart participates in CSS layout/paint,
// carrying the author's classes) hosts a QtGraphs GraphsView as a foreign fill — QtGraphs (not
// QtCharts, which crashes offscreen) is the modern Qt Quick charts module and loads as a runtime
// plugin, so the loader needs no relink.
//
// The transpiler emits:  W.Chart { cssClass: […]; categories: [...]; values: [...]; [barColor]; [axisMax] }
import QtQuick
import QtGraphs
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property var categories: []
    property var values: []
    property color barColor: "#41cd52"
    property real axisMax: 0   // 0 → auto (max value + 10%)

    cssPrimitive: "div"
    implicitWidth: 320
    implicitHeight: 220

    GraphsView {
        anchors.fill: parent
        theme: GraphsTheme { colorScheme: GraphsTheme.ColorScheme.Dark }
        axisX: BarCategoryAxis { categories: root.categories }
        axisY: ValueAxis {
            min: 0
            max: root.axisMax > 0 ? root.axisMax
                 : (root.values.length ? Math.max.apply(null, root.values) * 1.1 : 1)
        }
        BarSeries {
            BarSet { values: root.values; color: root.barColor }
        }
    }
}
