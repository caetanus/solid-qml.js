// Button — the <button> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component; the transpiler INSTANTIATES these, it no longer hand-emits the button's internals).
// Extends the engine's CssFill so it participates in CSS layout/paint; owns hover state + click.
//
// The transpiler emits:  W.Button { cssClass: […]; text: "Save"; onClicked: <handler>; <children> }
import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    // The label text (text children of <button>). Element children (e.g. a nested <img>) append to
    // the default `data` and lay out alongside the label, exactly like the old inline emit.
    property string text: ""
    signal clicked()

    cssPrimitive: "button"
    // Hover mirrors into cssState so `:hover` rules restyle the button (and, via ancestor scoping,
    // its label) natively — the MouseArea doubles as the hover tracker.
    cssState: __ma.containsMouse ? ["hover"] : []

    Css.CssText {
        cssPrimitive: "text"
        text: root.text
        visible: root.text.length > 0
    }
    MouseArea {
        id: __ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
