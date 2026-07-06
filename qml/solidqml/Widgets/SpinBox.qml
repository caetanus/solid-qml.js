// SpinBox — the <input type="number"> component in solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). A T.SpinBox with CSS support: wrapper CssFill (cssPrimitive "input") + the
// editable control (background null), a plain TextInput contentItem with CSS-bridged colour/font, and
// up/down indicators as `.spin-up`/`.spin-down` CssFills whose `+`/`−` glyphs use the CLOBBER host
// pattern (a plain Text inside an anchors.fill Item, styled via a nested CssItem — a Css child would
// be re-laid-out by the engine and lose its centerIn anchor).
//
// `value` is a two-way alias so the emit's controlled RestoreNone Binding (target __inputN, property
// "value") writes through it; `from`/`to`/`stepSize` are aliases set by the emit from min/max/step.
// User edits relay via the `valueModified` signal (excludes Binding re-assertions — no echo loop).
// The focused wheel steps the value and re-fires valueModified() so the author's onChange wiring runs.
//
// The transpiler emits:  W.SpinBox { id: __inputN; cssClass: […]; from; to; stepSize; [enabled:false];
//                                    onValueModified; controlled Binding on value }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias value: ctl.value
    property alias from: ctl.from
    property alias to: ctl.to
    property alias stepSize: ctl.stepSize
    signal valueModified()

    cssPrimitive: "input"
    cssState: (ctl.activeFocus ? ["focus"] : []).concat(!ctl.enabled ? ["disabled"] : [])
    implicitWidth: ctl.implicitWidth
    implicitHeight: ctl.implicitHeight

    T.SpinBox {
        id: ctl
        anchors.fill: parent
        background: null
        editable: true
        activeFocusOnTab: solidTabstop.enabled
        onValueModified: root.valueModified()
        // Controls resize contentItem to the control minus paddings — without a rightPadding the
        // TextInput covers the +/- buttons and eats their clicks.
        leftPadding: 12
        rightPadding: 32
        // HTML semantics: the wheel steps the value, but ONLY while the field has focus; unfocused,
        // the event must fall through to the page scroll. valueModified() reuses the onChange wiring.
        // Stepping writes `value` directly: Qt 6.11's SpinBox refactor (QQuickAbstractSpinBox) dropped
        // the Q_INVOKABLE from the increment/decrement methods. acceptedDevices: the default is Mouse ONLY —
        // touchpad scrolling is filtered in wantsPointerEvent; accumulate to the 120-unit notch.
        WheelHandler {
            property real __acc: 0
            enabled: ctl.activeFocus
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ctl.value = Math.max(ctl.from, Math.min(ctl.to, ctl.value + s * ctl.stepSize)); ctl.valueModified() } }
        }
        // contentItem: a plain TextInput (not Css) — it lives inside the control's item tree, not our
        // CSS layout engine. Color/font are bridged from the CssFill wrapper (root).
        contentItem: TextInput {
            // T.SpinBox is a focus scope: focus: true forwards the control's active focus into the
            // TextInput so tabbing in lets the user type immediately.
            focus: true
            text: ctl.displayText
            validator: ctl.validator
            readOnly: !ctl.editable
            color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
            font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
            font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            selectByMouse: true
        }
        // up indicator: Css.CssFill at the top-right of the SpinBox; cssState "active" when pressed.
        // Inset 2px from the wrapper's edge so the buttons sit INSIDE the rounded border.
        up.indicator: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["spin-up"]
            cssState: ctl.up.pressed ? ["active"] : []
            x: parent.width - width - 2
            y: 2
            width: 24
            height: (parent.height - 4) / 2
            implicitWidth: 24
            implicitHeight: (parent.height - 4) / 2
            // Plain Text, NOT CssText: a Css child inside this CssFill is re-laid-out by the CSS engine
            // (stomps the centerIn anchor). A plain primitive is invisible to the layout; the nested
            // CssItem injects the CSS (color/font from the .spin-glyph rule) without joining the layout.
            Item {
                anchors.fill: parent
                Text {
                    text: "+"
                    anchors.centerIn: parent
                    Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }
                }
            }
        }
        // down indicator: mirrors up, at the bottom-right (same 2px inset).
        down.indicator: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["spin-down"]
            cssState: ctl.down.pressed ? ["active"] : []
            x: parent.width - width - 2
            y: parent.height / 2
            width: 24
            height: (parent.height - 4) / 2
            implicitWidth: 24
            implicitHeight: (parent.height - 4) / 2
            Item {
                anchors.fill: parent
                Text {
                    text: "−"
                    anchors.centerIn: parent
                    Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }
                }
            }
        }
    }
}
