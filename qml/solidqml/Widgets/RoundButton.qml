// RoundButton — the <RoundButton> component in module solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the button).
// Same shape as Button.qml (the wrapper CssFill IS the painted button surface + the CssText label is
// a CSS layout child, so `.x button text` scoping keeps working) but the interaction comes from a
// real T.RoundButton filling the wrapper. The control's own background/contentItem are nulled: a
// slot background cannot participate in the author's flex layout, so the wrapper doubles as it —
// carrying cssPrimitive "button". The extra "round" class the spec assigns is passed by the emit.
//
// The transpiler emits:  W.RoundButton { cssClass: [..., "round"]; text: "…"; onClicked: <body> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property string text: ""
    property bool disabled: false
    signal clicked()

    cssPrimitive: "button"
    cssState: (__ctl.hovered ? ["hover"] : []).concat(__ctl.pressed ? ["active"] : []).concat(__ctl.activeFocus ? ["focus"] : []).concat(!__ctl.enabled ? ["disabled"] : [])

    Css.CssText {
        cssPrimitive: "text"
        text: root.text
    }
    T.RoundButton {
        id: __ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        hoverEnabled: true
        enabled: !root.disabled
        background: null
        contentItem: null
        onClicked: root.clicked()
    }
}
