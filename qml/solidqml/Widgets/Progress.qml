// Progress — the <progress> component in solidqml.Widgets. A T.ProgressBar with CSS support:
// Templates supplies the behaviour, our Css items paint every pixel (track + bar). `value < 0`
// (the default, i.e. no value given) means indeterminate — a 30% segment sliding in a loop.
//
// The transpiler emits:  W.Progress { cssClass: […]; max: <n>; value: <expr> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property real value: -1
    property real max: 1

    cssPrimitive: "progress"
    cssState: bar.indeterminate ? ["indeterminate"] : []
    // Fixed implicit size: with chrome nulled the control reports 0×0 (Templates have no
    // implicit-size policy — that's the style's job, and we ARE the style). CSS overrides.
    implicitWidth: 200
    implicitHeight: 8

    T.ProgressBar {
        id: bar
        anchors.fill: parent
        contentItem: null
        background: null
        from: 0
        to: root.max
        indeterminate: root.value < 0
        value: root.value < 0 ? 0 : root.value

        // Track: fills the control; author styles via `.track` (background, radius).
        Css.CssRect {
            cssPrimitive: ""
            cssClass: ["track"]
            anchors.fill: parent
            // Bar: the covered portion. A PLAIN Rectangle inside an anchored Item host — a Css
            // child's width binding is CLOBBERED by the layout engine (block child stretches to
            // 100%); a plain item is not a layout child, so the visualPosition binding holds. The
            // nested CssItem paints `.bar`. Indeterminate: a 30% segment sliding across in a loop.
            Item {
                anchors.fill: parent
                Rectangle {
                    width: bar.indeterminate ? parent.width * 0.3 : bar.visualPosition * parent.width
                    height: parent.height
                    color: "#176b87"
                    Css.CssItem { cssPrimitive: "rect"; cssClass: ["bar"] }
                    NumberAnimation on x {
                        running: bar.indeterminate
                        from: 0
                        to: bar.width * 0.7
                        duration: 1200
                        loops: Animation.Infinite
                    }
                }
            }
        }
    }
}
