// Chart — a COMPLEX 2D chart, its own opt-in module solidqml.Widgets.Chart (owner directives
// 2026-07-06: "use CssFill for QtGraphs"; per-widget modules "import Solid.Widgets.Surface"; "não
// quero bar nos charts, quero charts realmente complexos" — bars we can already draw with the CSS
// engine, so QtGraphs earns its keep only for the hard stuff). A CssFill "div" wrapper hosts a
// QtGraphs GraphsView as a foreign fill: a filled area under a smooth spline, a second spline, and a
// scatter overlay over shared value axes — the kind of multi-series plot you would NOT hand-roll.
// QtGraphs loads as a runtime plugin, imported ONLY here, so the core engine never depends on it.
//
// The transpiler emits:  WChart.Chart { cssClass: […] }
import QtQuick
import QtGraphs
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property real xMax: 10
    property color accent: "#41cd52"

    cssPrimitive: "div"
    implicitWidth: 520
    implicitHeight: 340

    GraphsView {
        id: gv
        anchors.fill: parent
        anchors.margins: 8
        theme: GraphsTheme {
            colorScheme: GraphsTheme.ColorScheme.Dark
            seriesColors: [root.accent, "#4aa3ff", "#ff8a3d", "#c46bff"]
        }
        axisX: ValueAxis { min: 0; max: root.xMax; subTickCount: 4 }
        axisY: ValueAxis { min: -1.5; max: 5 }

        AreaSeries {
            color: "#2241cd52"; borderColor: root.accent; borderWidth: 2
            upperSeries: LineSeries { id: area }
        }
        SplineSeries { id: wave; width: 3 }
        SplineSeries { id: ripple; width: 2 }
        ScatterSeries {
            id: samples
            pointDelegate: Rectangle {
                width: 9; height: 9; radius: 4.5
                color: "#ff8a3d"; border.color: "#0f1720"; border.width: 1
            }
        }

        Component.onCompleted: {
            for (var x = 0; x <= root.xMax; x += 0.25) {
                area.append(x, 2.2 + 1.6 * Math.sin(x * 0.7));
                wave.append(x, 2.5 + 2.0 * Math.sin(x));
                ripple.append(x, 1.5 + 1.2 * Math.cos(x * 0.8) + 0.3 * Math.sin(x * 3));
            }
            for (var k = 0; k <= root.xMax; k += 1)
                samples.append(k, 2.5 + 2.0 * Math.sin(k) + (Math.random() - 0.5) * 0.6);
        }
    }
}
