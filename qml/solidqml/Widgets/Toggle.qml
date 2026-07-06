// Toggle — the <input type="checkbox" role="switch"> component in solidqml.Widgets (owner directive
// 2026-07-05: one .qml per component). A T.Switch with CSS support: the wrapper CssFill (cssPrimitive
// "input") carries CSS identity + focus/checked/disabled state; the indicator slot hosts a track
// CssFill (36×20) containing a knob (16×16) whose x is animated by T.Switch.visualPosition (0→1).
//
// Knob CLOBBER fix: a Css child's width/geometry binding is overwritten by the layout engine (block
// child stretches to 100%); the knob is a PLAIN Rectangle inside an anchors.fill Item host (not a
// layout child) with a nested CssItem painting the .knob rule, so the position binding holds. The
// Behavior on x gives a 120 ms slide (CSS transitions are not yet wired).
//
// `checked` is a two-way alias (controlled Binding writes through); user toggles relay via `toggled`.
//
// The transpiler emits:  W.Toggle { id: __inputN; cssClass: […]; [enabled: false]; onToggled; Binding }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias checked: sw.checked
    signal toggled()

    readonly property var __state: (sw.activeFocus ? ["focus"] : []).concat(sw.checked ? ["checked"] : []).concat(!sw.enabled ? ["disabled"] : [])

    cssPrimitive: "input"
    cssState: __state
    implicitWidth: sw.implicitWidth
    implicitHeight: sw.implicitHeight

    T.Switch {
        id: sw
        anchors.fill: parent
        background: null
        contentItem: null
        activeFocusOnTab: solidTabstop.enabled
        onToggled: root.toggled()
        Keys.onDownPressed: { if (solidTabstop.enabled) { var __n = sw.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onRightPressed: { if (solidTabstop.enabled) { var __n = sw.nextItemInFocusChain(true); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onUpPressed: { if (solidTabstop.enabled) { var __n = sw.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }
        Keys.onLeftPressed: { if (solidTabstop.enabled) { var __n = sw.nextItemInFocusChain(false); if (__n) __n.forceActiveFocus(Qt.TabFocusReason) } }

        indicator: Css.CssFill {
            cssPrimitive: "span"
            cssClass: ["track"]
            cssState: root.__state
            width: 36
            height: 20
            implicitWidth: 36
            implicitHeight: 20
            // Anchored Item host insulates the knob from the CSS flex pass (see the header note); the
            // geometry bindings live on the plain Rectangle inside. The nested CssItem injects
            // background-color/radius/border from the .knob rule.
            Item {
                anchors.fill: parent
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    color: "#ffffff"
                    y: (parent.height - height) / 2
                    // visualPosition goes 0→1 as the switch toggles; multiply by the remaining track width.
                    x: sw.visualPosition * (parent.width - width)
                    Behavior on x { NumberAnimation { duration: 120 } }
                    Css.CssItem { cssPrimitive: "rect"; cssClass: ["knob"] }
                }
            }
        }
    }
}
