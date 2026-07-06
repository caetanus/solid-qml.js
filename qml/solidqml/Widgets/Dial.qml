// Dial — the <Dial> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component; the transpiler INSTANTIATES these, it no longer hand-emits the control). A T.Dial with
// CSS support. T.Dial positions NOTHING (qquickdial_p.h): the STYLE places the handle from `angle`
// (0° = 12 o'clock, positive clockwise). We compute the point directly: cx + sin(angle)·r,
// cy − cos(angle)·r with r = background.width/2 − 12. The dial circle is the background slot
// (border-radius via CSS).
//
// The transpiler emits:  W.Dial { id: __inputN; cssClass; from; to; stepSize; onMoved; disabled }
// with a RestoreNone Binding on `value` for the controlled value. `value` is a two-way alias to
// the control, so `__inputN.value` reads AND the Binding writes through it.
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property real from: 0
    property real to: 100
    property real stepSize: 1
    property alias value: __ctl.value
    property bool disabled: false
    signal moved()

    cssPrimitive: ""
    cssState: (__ctl.activeFocus ? ["focus"] : []).concat(__ctl.pressed ? ["active"] : []).concat(!__ctl.enabled ? ["disabled"] : [])
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.Dial {
        id: __ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        from: root.from
        to: root.to
        stepSize: root.stepSize
        enabled: !root.disabled
        onMoved: root.moved()
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        // The dial face: a centred square CssFill — `.dial { border-radius: … }` makes it a circle.
        background: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["dial"]
            x: __ctl.width / 2 - width / 2
            y: __ctl.height / 2 - height / 2
            width: Math.max(32, Math.min(__ctl.width, __ctl.height))
            height: width
            implicitWidth: 96
            implicitHeight: 96
        }
        handle: Css.CssRect {
            cssClass: ["handle"]
            width: 12
            height: 12
            implicitWidth: 12
            implicitHeight: 12
            x: __ctl.background.x + __ctl.background.width / 2 - width / 2 + Math.sin(__ctl.angle * Math.PI / 180) * (__ctl.background.width / 2 - 12)
            y: __ctl.background.y + __ctl.background.height / 2 - height / 2 - Math.cos(__ctl.angle * Math.PI / 180) * (__ctl.background.width / 2 - 12)
        }
    }
}
