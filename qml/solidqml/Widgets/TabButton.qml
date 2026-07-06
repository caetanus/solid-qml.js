// TabButton — a single tab in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A T.TabButton with CSS support, only ever instantiated as a child of W.TabBar (its
// root lands in the TabBar's contentData → contentModel). Background and contentItem are SIBLING
// slots, so ancestor-state scoping cannot reach the label through the background — both carry the
// same cssState (checked → "selected", hovered → "hover").
//
// The transpiler emits:  W.TabButton { text: "…" }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.TabButton {
    id: ctl
    // `text` is inherited from T.AbstractButton (a FINAL property — must not be redeclared); the
    // transpiler sets it directly (W.TabButton { text: "…" }).

    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    padding: 8
    activeFocusOnTab: solidTabstop.enabled

    background: Css.CssFill {
        cssPrimitive: "div"
        cssClass: ["tab"]
        cssState: (ctl.checked ? ["selected"] : []).concat(ctl.hovered ? ["hover"] : [])
    }
    contentItem: Css.CssText {
        cssPrimitive: ""
        cssClass: ["tab-label"]
        cssState: (ctl.checked ? ["selected"] : []).concat(ctl.hovered ? ["hover"] : [])
        text: ctl.text
    }
}
