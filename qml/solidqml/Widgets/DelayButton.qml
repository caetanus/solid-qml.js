// DelayButton — the <DelayButton> component in module solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the control).
// A T.DelayButton with CSS support. The wrapper stays paint-less (cssPrimitive ""): the `.delay`
// background slot owns the pill so a generic `button {}` rule can't double-paint it. The progress
// overlay lives in an anchored Item host inside the background (its width binding must survive the
// flex pass). Both wrapper and background carry the full button state list so `.delay:active` /
// `:checked` restyle the pill directly.
//
// The transpiler emits:  W.DelayButton { cssClass: […]; delay: <ms>; text: "…"; onActivated: <body> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property string text: ""
    property int delay: 300
    property bool disabled: false
    signal activated()

    cssPrimitive: ""
    cssState: (__ctl.hovered ? ["hover"] : []).concat(__ctl.pressed ? ["active"] : []).concat(__ctl.checked ? ["checked"] : []).concat(__ctl.activeFocus ? ["focus"] : []).concat(!__ctl.enabled ? ["disabled"] : [])
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.DelayButton {
        id: __ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        hoverEnabled: true
        delay: root.delay
        enabled: !root.disabled
        // T.DelayButton has no built-in progress animation: without a `transition`, pressing sets
        // `progress` straight to 1.0 → the button arms on a single click. This is the Basic style's
        // transition (hold ramps 0→1 over `delay`; release eases back). Same class of bug as <Drawer>.
        transition: Transition { NumberAnimation { duration: __ctl.delay * (__ctl.pressed ? 1.0 - __ctl.progress : 0.3 * __ctl.progress) } }
        horizontalPadding: 16
        verticalPadding: 8
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        onActivated: root.activated()
        background: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["delay"]
            cssState: root.cssState
            implicitWidth: 120
            implicitHeight: 36
            // Progress overlay: grows with the hold (progress 0→1 over `delay` ms).
            Item {
                anchors.fill: parent
                Css.CssRect {
                    cssClass: ["delay-fill"]
                    width: __ctl.progress * parent.width
                    height: parent.height
                }
            }
        }
        // Label: the control sizes/positions its contentItem; colour/font inherit from the wrapper.
        contentItem: Css.CssText {
            cssPrimitive: "text"
            text: root.text
        }
    }
}
