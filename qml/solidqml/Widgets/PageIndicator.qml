// PageIndicator — the <PageIndicator> component in module solidqml.Widgets (owner directive
// 2026-07-05: one .qml per component). A T.PageIndicator with CSS support: wrapper CssFill
// (cssPrimitive "pageindicator") + the control. The dot delegate is a Css.CssRect ["dot"] 8×8 with
// cssState "selected" on the current page; Templates create NO contentItem → the Basic-style Row +
// Repeater is supplied here. `count`/`currentIndex` are aliases to the control.
//
// The transpiler emits:  W.PageIndicator { cssClass: […]; count: <n>; currentIndex: <expr> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias count: dots.count
    property alias currentIndex: dots.currentIndex

    cssPrimitive: "pageindicator"
    implicitWidth: dots.implicitWidth
    implicitHeight: dots.implicitHeight

    T.PageIndicator {
        id: dots
        anchors.fill: parent
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        background: null
        spacing: 6
        delegate: Css.CssRect {
            required property int index
            cssPrimitive: "div"
            cssClass: ["dot"]
            cssState: index === dots.currentIndex ? ["selected"] : []
            implicitWidth: 8
            implicitHeight: 8
        }
        contentItem: Row {
            spacing: dots.spacing
            Repeater {
                model: dots.count
                delegate: dots.delegate
            }
        }
    }
}
