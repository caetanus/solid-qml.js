// Checkbox — the <input type="checkbox"> component in solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component). A T.CheckBox with CSS support: the wrapper CssFill (cssPrimitive "input")
// carries CSS identity + focus/checked/disabled state; the indicator slot hosts a fixed-size (20×20)
// CssFill with a centred glyph (✓) visible when checked. Geometry is hardcoded (the indicator is NOT
// in a Css layout container); glyph colour/background ARE CSS-styleable via .indicator rules.
//
// `checked` is a two-way alias so the emit's controlled RestoreNone Binding (target __inputN, property
// "checked") writes through it; user toggles relay via the `toggled` signal so the author's onChange
// doesn't echo on the Binding re-assertion. `disabled` maps to the instance's inherited enabled:false.
//
// The transpiler emits:  W.Checkbox { id: __inputN; cssClass: […]; [enabled: false]; onToggled; Binding }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias checked: box.checked
    signal toggled()

    // Shared state list for the wrapper and the indicator (checked/focus/disabled pseudo-classes).
    readonly property var __state: (box.activeFocus ? ["focus"] : []).concat(box.checked ? ["checked"] : []).concat(!box.enabled ? ["disabled"] : [])

    cssPrimitive: "input"
    cssState: __state
    implicitWidth: box.implicitWidth
    implicitHeight: box.implicitHeight

    T.CheckBox {
        id: box
        anchors.fill: parent
        background: null
        contentItem: null
        activeFocusOnTab: solidTabstop.enabled
        onToggled: root.toggled()
        // Arrows move focus along the chain (desktop dialog semantics), same opt-out as Tab.
        Keys.onDownPressed: { if (solidTabstop.enabled) { var __n = box.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onRightPressed: { if (solidTabstop.enabled) { var __n = box.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onUpPressed: { if (solidTabstop.enabled) { var __n = box.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onLeftPressed: { if (solidTabstop.enabled) { var __n = box.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }

        // indicator: a fixed-size Css item (not in a Css layout container — geometry is hardcoded).
        indicator: Css.CssFill {
            cssPrimitive: "span"
            cssClass: ["indicator"]
            cssState: root.__state
            width: 20
            height: 20
            implicitWidth: 20
            implicitHeight: 20
            // Anchored Item host: once the indicator's CSS carries box rules (border etc.) the layout
            // engine runs a flex pass over contentHolder children and pins plain children top-left; an
            // anchors.fill Item is skipped, and anchors hold inside it. The nested CssItem injects
            // color/font from the .indicator-glyph rule without joining any layout.
            Item {
                anchors.fill: parent
                Text {
                    text: "✓"
                    visible: box.checked
                    anchors.centerIn: parent
                    Css.CssItem { cssPrimitive: "text"; cssClass: ["indicator-glyph"] }
                }
            }
        }
    }
}
