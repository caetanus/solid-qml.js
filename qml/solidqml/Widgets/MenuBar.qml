// MenuBar — the <MenuBar> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component). A T.MenuBar (a QQuickContainer) with CSS support: a CssFill "div" wrapper carrying
// the author's classes, mirroring the control's implicit size, over the canonical Basic-style
// structure — a Row+Repeater contentItem over `menuBar.contentModel` and a `.menubar` background.
//
// The author's <Menu title> children become W.MenuBarItem entries routed into the control's
// contentData via the default `barItems` alias — qquickmenubar appends MenuBarItems directly ("you
// can add MenuBarItems directly to the menu bar"), wiring hover-open/click-open.
//
// The transpiler emits:  W.MenuBar { cssClass: […]; <W.MenuBarItem> children }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    default property alias barItems: bar.contentData

    cssPrimitive: "div"
    // Best-effort defaults: once the author's CSS gives the wrapper box rules, the engine's content
    // pass measures the plain T.MenuBar as 0 and OVERWRITES these — size the box in CSS then.
    implicitWidth: bar.implicitWidth
    implicitHeight: bar.implicitHeight

    T.MenuBar {
        id: bar
        anchors.fill: parent
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        contentItem: Row {
            spacing: bar.spacing
            Repeater { model: bar.contentModel }
        }
        background: Css.CssFill {
            cssPrimitive: "div"
            cssClass: ["menubar"]
        }
    }
}
