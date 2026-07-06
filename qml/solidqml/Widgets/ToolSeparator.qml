// ToolSeparator — the <ToolSeparator> component in module solidqml.Widgets (owner directive
// 2026-07-05: one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits
// the control's internals). A T.ToolSeparator with CSS support: the visible rule is a CssRect
// ["sep"] centred in an Item content host (1px wide, 60% of the available height). The host
// insulates the centring anchors from any CSS pass and gives the control its implicit metrics.
//
// The transpiler emits:  W.ToolSeparator { cssClass: […] }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    cssPrimitive: ""
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.ToolSeparator {
        id: __ctl
        anchors.fill: parent
        background: null
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        contentItem: Item {
            implicitWidth: 9
            implicitHeight: 28
            Css.CssRect {
                cssClass: ["sep"]
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: parent.height * 0.6
            }
        }
    }
}
