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
    // Desktop model (tab-focus study §6): a button IS a tab stop and is activated by Space/Enter as
    // well as click. Focus lives on the MouseArea — a QtQuick item that exposes activeFocusOnTab
    // (the engine's Css types don't, by QML registration revision) — and hover + focus mirror into
    // cssState so `:hover`/`:focus` rules restyle the button (and its label) natively.
    cssState: (__ma.containsMouse ? ["hover"] : []).concat(__ma.activeFocus ? ["focus"] : [])

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
        activeFocusOnTab: solidTabstop.enabled
        onClicked: root.clicked()
        Keys.onSpacePressed: root.clicked()
        Keys.onReturnPressed: root.clicked()
        Keys.onEnterPressed: root.clicked()
    }
}
