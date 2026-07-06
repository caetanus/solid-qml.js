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
    // The default button (<button type="submit">): the enclosing Dialog fires it on Enter (study §6).
    // Surfaced as a `default` cssState so `button:default { … }` can emphasise it like a desktop toolkit.
    property bool isDefault: false
    signal clicked()

    // Move keyboard focus onto the button (its MouseArea holds the focus, not the CssFill root). Used
    // by Dialog to focus the default button on open, so Enter confirms it (study §6).
    function takeFocus() { __ma.forceActiveFocus(Qt.TabFocusReason); }

    cssPrimitive: "button"
    // Desktop model (tab-focus study §6): a button IS a tab stop and is activated by Space/Enter as
    // well as click. Focus lives on the MouseArea — a QtQuick item that exposes activeFocusOnTab
    // (the engine's Css types don't, by QML registration revision) — and hover + focus mirror into
    // cssState so `:hover`/`:focus` rules restyle the button (and its label) natively.
    cssState: (__ma.containsMouse ? ["hover"] : []).concat(__ma.activeFocus ? ["focus"] : [])
                                                  .concat(root.isDefault ? ["default"] : [])

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
        // Desktop model (study §6): clicking a control also FOCUSES it, so it becomes the tab anchor
        // and Tab continues from the clicked button (a bare MouseArea doesn't take focus on its own).
        onPressed: if (solidTabstop.enabled) __ma.forceActiveFocus(Qt.MouseFocusReason)
        onClicked: root.clicked()
        Keys.onSpacePressed: root.clicked()
        Keys.onReturnPressed: root.clicked()
        Keys.onEnterPressed: root.clicked()
    }
}
