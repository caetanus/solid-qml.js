// RangeSlider — the <RangeSlider> component in module solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the control).
// A T.RangeSlider with CSS support.
//
// T.RangeSlider has NO value/handle of its own: `first` and `second` are sub-objects
// (QQuickRangeSliderNode, qquickrangeslider_p.h) each carrying value / visualPosition / handle /
// moved(). Track and handle geometry copy the Slider; the range fill spans
// [first.visualPosition, second.visualPosition]. The fill lives in an anchored Item host: unlike
// the Slider's zero-based fill, its x offset must survive the CSS flex pass that runs over the
// track's Css children once `.track` carries box rules.
//
// The transpiler emits:  W.RangeSlider { id: __inputN; cssClass; from; to; stepSize; onMoved;
// disabled } with a RestoreNone Binding per node on `.value`. `first`/`second` are aliases to the
// control's sub-nodes, so `__inputN.first.value` reads AND `Binding { target: __inputN.first }`
// writes across the component boundary; `moved()` fires from BOTH nodes so onChange runs either way.
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property real from: 0
    property real to: 100
    property real stepSize: 1
    property alias first: __ctl.first
    property alias second: __ctl.second
    property bool disabled: false
    signal moved()

    cssPrimitive: "input"
    cssState: (__ctl.activeFocus ? ["focus"] : []).concat(!__ctl.enabled ? ["disabled"] : [])
    implicitWidth: __ctl.implicitWidth
    implicitHeight: __ctl.implicitHeight

    T.RangeSlider {
        id: __ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        from: root.from
        to: root.to
        stepSize: root.stepSize
        enabled: !root.disabled
        // Style-side implicit size: background/handles carry the natural metrics (Basic idiom).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, first.implicitHandleWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, first.implicitHandleHeight + topPadding + bottomPadding)
        // moved() fires from BOTH nodes so the author's onChange runs whichever handle drags/steps.
        first.onMoved: root.moved()
        second.onMoved: root.moved()
        // Track: same geometry as the Slider (6px tall, centred within the control).
        background: Css.CssFill {
            cssPrimitive: ""
            cssClass: ["track"]
            x: __ctl.leftPadding
            y: __ctl.topPadding + (__ctl.availableHeight - height) / 2
            width: __ctl.availableWidth
            height: 6
            implicitWidth: 200
            implicitHeight: 6
            // Anchored Item host: the range fill starts at first.visualPosition (x ≠ 0) — a bare Css
            // child would be re-laid-out to x 0 by the CSS flex pass over the styled track.
            Item {
                anchors.fill: parent
                Css.CssRect {
                    cssClass: ["track-fill"]
                    x: __ctl.first.visualPosition * parent.width
                    width: (__ctl.second.visualPosition - __ctl.first.visualPosition) * parent.width
                    height: parent.height
                }
            }
        }
        // Handles: the Slider's handle geometry, one per node, positioned by the node's own
        // visualPosition. No Behavior — dragging must be 1:1.
        first.handle: Css.CssRect {
            cssClass: ["handle"]
            width: 18
            height: 18
            implicitWidth: 18
            implicitHeight: 18
            x: __ctl.leftPadding + __ctl.first.visualPosition * (__ctl.availableWidth - width)
            y: __ctl.topPadding + __ctl.availableHeight / 2 - height / 2
        }
        second.handle: Css.CssRect {
            cssClass: ["handle"]
            width: 18
            height: 18
            implicitWidth: 18
            implicitHeight: 18
            x: __ctl.leftPadding + __ctl.second.visualPosition * (__ctl.availableWidth - width)
            y: __ctl.topPadding + __ctl.availableHeight / 2 - height / 2
        }
        // Focused wheel steps the FIRST handle (the Slider's accumulator, re-fires first.moved()).
        WheelHandler {
            property real __acc: 0
            enabled: __ctl.activeFocus
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { __ctl.first.value = Math.max(__ctl.from, Math.min(__ctl.to, __ctl.first.value + s * __ctl.stepSize)); __ctl.first.moved() } }
        }
    }
}
