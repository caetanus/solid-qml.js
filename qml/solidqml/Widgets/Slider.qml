// Slider — the <input type="range"> component in solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). A T.Slider with CSS support: wrapper CssFill (cssPrimitive "input") + the
// control, whose background is a Basic-style track CssFill (`.track`) holding a `.track-fill` CssRect
// that grows with visualPosition, and whose handle is a `.handle` CssRect (18×18) positioned by the
// slider's own geometry helpers — no Behavior, so dragging follows the pointer 1:1.
//
// `value` is a two-way alias so the emit's controlled RestoreNone Binding (target __inputN, property
// "value") writes through it; user drags relay via the `moved` signal (fires on positional change,
// NOT on Binding re-assertion). `from`/`to`/`stepSize` are aliases set by the emit from min/max/step.
// The focused wheel steps the value and re-fires moved() so the author's onInput/onChange wiring runs.
//
// The transpiler emits:  W.Slider { id: __inputN; cssClass: […]; from; to; stepSize; [enabled:false];
//                                   onMoved; controlled Binding on value }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias value: ctl.value
    property alias from: ctl.from
    property alias to: ctl.to
    property alias stepSize: ctl.stepSize
    signal moved()

    cssPrimitive: "input"
    cssState: (ctl.activeFocus ? ["focus"] : []).concat(!ctl.enabled ? ["disabled"] : [])
    implicitWidth: ctl.implicitWidth
    implicitHeight: ctl.implicitHeight

    T.Slider {
        id: ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        onMoved: root.moved()
        // Track: background slot (fills the control); centred vertically via explicit x/y bindings.
        // Qt Basic-style geometry so the 6px-tall track sits in the middle of the taller handle.
        background: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["track"]
            x: ctl.leftPadding
            y: ctl.topPadding + (ctl.availableHeight - height) / 2
            width: ctl.availableWidth
            height: 6
            implicitHeight: 6
            // Progress fill: a CssRect inside the track growing with visualPosition.
            Css.CssRect {
                cssClass: ["track-fill"]
                width: ctl.visualPosition * parent.width
                height: parent.height
            }
        }
        // Handle: a CssRect (18×18) positioned by the slider's own geometry helpers. No Behavior —
        // dragging must follow the pointer 1:1 (no snap animation like the Switch knob).
        handle: Css.CssRect {
            cssClass: ["handle"]
            width: 18
            height: 18
            implicitWidth: 18
            implicitHeight: 18
            x: ctl.leftPadding + ctl.visualPosition * (ctl.availableWidth - width)
            y: ctl.topPadding + ctl.availableHeight / 2 - height / 2
        }
        // Focused wheel steps the value (same semantics as the SpinBox); moved() re-fires so the
        // author's onInput/onChange wiring runs. Same touchpad handling as the SpinBox wheel:
        // Mouse-only default + 120-unit notch accumulation.
        WheelHandler {
            property real __acc: 0
            enabled: ctl.activeFocus
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ctl.value = Math.max(ctl.from, Math.min(ctl.to, ctl.value + s * ctl.stepSize)); ctl.moved() } }
        }
    }
}
