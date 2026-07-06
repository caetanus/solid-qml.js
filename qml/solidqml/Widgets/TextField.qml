// TextField — the <input type="text|email|password|search"> component in solidqml.Widgets (owner
// directive 2026-07-05: one .qml per component). A T.TextField with CSS support: the wrapper CssFill
// (cssPrimitive "input") carries CSS identity + `:focus`/`:disabled` state; the inner control paints
// its text with CSS-bridged colour/font and hosts the placeholder overlay.
//
// `text` is a two-way alias so the emit's controlled RestoreNone Binding (target __inputN, property
// "text") writes through it. User edits relay via the `textEdited` / `editingFinished` / `keyPressed`
// signals so the author's onInput/onChange/onKeyDown handlers fire WITHOUT echoing on Binding
// re-assertion (textChanged would). `disabled` maps to the instance's inherited `enabled: false`,
// which propagates to the control (cssState reads field.enabled).
//
// The transpiler emits:  W.TextField { id: __inputN; cssClass: […]; placeholder; echoMode; readOnly;
//                                      maximumLength; onTextEdited; onEditingFinished; onKeyPressed;
//                                      Binding on text }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias text: field.text
    property alias placeholder: ph.text
    property alias echoMode: field.echoMode
    property alias readOnly: field.readOnly
    property alias maximumLength: field.maximumLength
    // Relay the control's user-edit signals (NOT text/checkedChanged) so handlers don't echo on
    // the controlled Binding re-assertion. keyPressed forwards the KeyEvent for onKeyDown wiring.
    signal textEdited()
    signal editingFinished()
    signal keyPressed(var event)

    cssPrimitive: "input"
    cssState: (field.activeFocus ? ["focus"] : []).concat(!field.enabled ? ["disabled"] : [])
    implicitWidth: field.implicitWidth
    implicitHeight: field.implicitHeight

    T.TextField {
        id: field
        anchors.fill: parent
        // No visual chrome from Templates; our CssFill owns every painted pixel.
        background: null
        // CSS-inherited colour/font bridged from the wrapper (root IS the CssFill).
        color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
        font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
        font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
        leftPadding: 12
        rightPadding: 12
        verticalAlignment: TextInput.AlignVCenter
        selectByMouse: true
        activeFocusOnTab: solidTabstop.enabled
        onTextEdited: root.textEdited()
        onEditingFinished: root.editingFinished()
        Keys.onPressed: (event) => root.keyPressed(event)

        // Placeholder: a plain Text overlay (positioned to match the text baseline). Hides when the
        // field has text or is focused (web `<input>` placeholder semantics). Empty text → invisible.
        Text {
            id: ph
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: parent.leftPadding
            visible: parent.text.length === 0 && !parent.activeFocus && ph.text.length > 0
            color: "#9aa0a6"
            font: parent.font
        }
    }
}
