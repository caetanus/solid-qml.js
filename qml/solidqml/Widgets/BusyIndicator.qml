// BusyIndicator — the <BusyIndicator> component in module solidqml.Widgets (owner directive
// 2026-07-05: one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits
// the spinner). A T.BusyIndicator with CSS support: eight plain Rectangles on a circle inside the
// contentItem Item host (a control slot — the CSS layout never sees it), opacity staggered 1/8…1,
// the HOST spun by a RotationAnimation gated on `running` (so an idle indicator costs zero frames).
// Each spoke nests a CssItem ["spoke"] that injects background-color/radius from CSS (the same
// injection idiom as the switch knob).
//
// The transpiler emits:  W.BusyIndicator { cssClass: […]; running: <expr> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property bool running: true
    cssPrimitive: ""
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.BusyIndicator {
        id: __ctl
        anchors.fill: parent
        running: root.running
        visible: running
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        contentItem: Item {
            implicitWidth: 40
            implicitHeight: 40
            property real __r: Math.min(width, height) / 2 - 5
            RotationAnimation on rotation {
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
                running: root.running
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.125
                x: parent.width / 2 + Math.cos(0 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(0 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.25
                x: parent.width / 2 + Math.cos(1 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(1 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.375
                x: parent.width / 2 + Math.cos(2 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(2 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.5
                x: parent.width / 2 + Math.cos(3 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(3 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.625
                x: parent.width / 2 + Math.cos(4 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(4 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.75
                x: parent.width / 2 + Math.cos(5 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(5 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 0.875
                x: parent.width / 2 + Math.cos(6 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(6 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
            Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"; opacity: 1
                x: parent.width / 2 + Math.cos(7 * Math.PI / 4) * parent.__r - width / 2
                y: parent.height / 2 + Math.sin(7 * Math.PI / 4) * parent.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
        }
    }
}
