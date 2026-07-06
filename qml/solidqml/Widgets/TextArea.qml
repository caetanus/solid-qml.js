// TextArea — the <textarea> component in solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A T.TextArea with CSS support: the wrapper CssFill (cssPrimitive "textarea") carries
// CSS identity + `:focus`/`:disabled` state; the inner control wraps its text with CSS-bridged
// colour/font and hosts the top-aligned placeholder overlay.
//
// T.TextArea extends TextEdit (no `textEdited` signal), so the author's onInput/onChange both map to
// the instance's `onTextChanged` — `text` is a two-way alias, so `textChanged` exists on the root and
// the controlled RestoreNone Binding (target __inputN, property "text") writes through it. The echo
// from the Binding re-assertion is harmless (QML only emits textChanged on an actual change).
//
// The transpiler emits:  W.TextArea { id: __inputN; cssClass: […]; placeholder; readOnly;
//                                     onTextChanged; Binding on text }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias text: field.text
    property alias placeholder: ph.text
    property alias readOnly: field.readOnly

    cssPrimitive: "textarea"
    cssState: (field.activeFocus ? ["focus"] : []).concat(!field.enabled ? ["disabled"] : [])
    implicitWidth: field.implicitWidth
    implicitHeight: field.implicitHeight

    T.TextArea {
        id: field
        anchors.fill: parent
        background: null
        // Allow the text to wrap; callers can override via CSS `white-space: nowrap` (not yet mapped).
        wrapMode: TextEdit.Wrap
        color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
        font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
        font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
        padding: 12
        selectByMouse: true
        activeFocusOnTab: solidTabstop.enabled

        // Placeholder: overlaid at the top-left of the editing area (top-aligned for multi-line).
        Text {
            id: ph
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.topMargin: parent.padding
            anchors.leftMargin: parent.padding
            visible: parent.text.length === 0 && !parent.activeFocus && ph.text.length > 0
            color: "#9aa0a6"
            font: parent.font
        }
    }
}
